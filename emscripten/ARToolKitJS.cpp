#include <stdio.h>
#include <AR/ar.h>
#include <AR/arMulti.h>
#include <emscripten.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <AR/config.h>
#include <AR/paramGL.h>
#include <AR/video.h>

struct multi_marker {
	int id;
	ARMultiMarkerInfoT *multiMarkerHandle;
};

struct arController {
	int id;

	ARParam param;
	ARParamLT *paramLT = NULL;

	ARUint8 *videoFrame = NULL;
	int videoFrameSize;

	int width = 0;
	int height = 0;

	ARHandle *arhandle = NULL;
	ARPattHandle *arPattHandle = NULL;
	ARMultiMarkerInfoT *arMultiMarkerHandle = NULL;
	AR3DHandle* ar3DHandle;

	ARdouble nearPlane = 0.0001;
	ARdouble farPlane = 1000.0;

	std::vector<multi_marker> multi_markers;

	int patt_id = 0; // Running pattern marker id

	ARdouble cameraLens[16];
    ARVideoLumaInfo *lumaInfo = NULL;
    AR_PIXEL_FORMAT pixFormat = AR_PIXEL_FORMAT_RGBA;
};

std::unordered_map<int, arController> arControllers;
std::unordered_map<int, ARParam> cameraParams;


// ============================================================================
//	Global variables
// ============================================================================

static ARdouble	gTransform[3][4];

static int gARControllerID = 0;
static int gCameraID = 0;

static int ARCONTROLLER_NOT_FOUND = -1;
static int MULTIMARKER_NOT_FOUND = -2;
static int MARKER_INDEX_OUT_OF_BOUNDS = -3;

static ARMarkerInfo gMarkerInfo;

extern "C" {

	/***************
	 * Set Log Level
	 ****************/
	void setLogLevel(int level) {
		arLogLevel = level;
	}

	int getLogLevel() {
		return arLogLevel;
	}

	/***********
	* Teardown *
	***********/

	void deleteHandle(arController *arc) {
		if (arc->arhandle != NULL) {
			arPattDetach(arc->arhandle);
			arDeleteHandle(arc->arhandle);
			arc->arhandle = NULL;
		}
		if (arc->ar3DHandle != NULL) {
			ar3DDeleteHandle(&(arc->ar3DHandle));
			arc->ar3DHandle = NULL;
		}
		if (arc->paramLT != NULL) {
			arParamLTFree(&(arc->paramLT));
			arc->paramLT = NULL;
		}
	}

	int teardown(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

        // Cleanup luma.
        arVideoLumaFinal(&(arc->lumaInfo));

		if (arc->videoFrame) {
			free(arc->videoFrame);
			arc->videoFrame = NULL;
			arc->videoFrameSize = 0;
		}

		deleteHandle(arc);

		arPattDeleteHandle(arc->arPattHandle);

		arControllers.erase(id);

		for (int i=0; i<arc->multi_markers.size(); i++) {
			arMultiFreeConfig(arc->multi_markers[i].multiMarkerHandle);
		}

		delete &arc->multi_markers;
		delete arc;

		return 0;
	}



	/*****************
	* Camera loading *
	*****************/

	int loadCamera(std::string cparam_name) {
		ARParam param;
		if (arParamLoad(cparam_name.c_str(), 1, &param) < 0) {
			ARLOGe("loadCamera(): Error loading parameter file %s for camera.\n", cparam_name.c_str());
			return -1;
		}
		int cameraID = gCameraID++;
		cameraParams[cameraID] = param;

		return cameraID;
	}

	int setCamera(int id, int cameraID) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		if (cameraParams.find(cameraID) == cameraParams.end()) { return -1; }

		arc->param = cameraParams[cameraID];

		if (arc->param.xsize != arc->width || arc->param.ysize != arc->height) {
			ARLOGw("*** Camera Parameter resized from %d, %d. ***\n", arc->param.xsize, arc->param.ysize);
			arParamChangeSize(&(arc->param), arc->width, arc->height, &(arc->param));
		}

		// ARLOGi("*** Camera Parameter ***\n");
		// arParamDisp(&(arc->param));

		deleteHandle(arc);

		if ((arc->paramLT = arParamLTCreate(&(arc->param), AR_PARAM_LT_DEFAULT_OFFSET)) == NULL) {
			ARLOGe("setCamera(): Error: arParamLTCreate.\n");
			return -1;
		}

		// ARLOGi("setCamera(): arParamLTCreated\n..%d, %d\n", (arc->paramLT->param).xsize, (arc->paramLT->param).ysize);

		// setup camera
		if ((arc->arhandle = arCreateHandle(arc->paramLT)) == NULL) {
			ARLOGe("setCamera(): Error: arCreateHandle.\n");
			return -1;
		}
		// AR_DEFAULT_PIXEL_FORMAT
		int set = arSetPixelFormat(arc->arhandle, arc->pixFormat);

		// ARLOGi("setCamera(): arCreateHandle done\n");

		arc->ar3DHandle = ar3DCreateHandle(&(arc->param));
		if (arc->ar3DHandle == NULL) {
			ARLOGe("setCamera(): Error creating 3D handle");
			return -1;
		}

		// ARLOGi("setCamera(): ar3DCreateHandle done\n");

		arPattAttach(arc->arhandle, arc->arPattHandle);
		// ARLOGi("setCamera(): Pattern handler attached.\n");

		arglCameraFrustumRH(&((arc->paramLT)->param), arc->nearPlane, arc->farPlane, arc->cameraLens);

		return 0;
	}




	/*****************
	* Marker loading *
	*****************/


	static int loadMarker(const char *patt_name, int *patt_id, ARHandle *arhandle, ARPattHandle **pattHandle_p) {
		// Loading only 1 pattern in this example.
		if ((*patt_id = arPattLoad(*pattHandle_p, patt_name)) < 0) {
			ARLOGe("loadMarker(): Error loading pattern file %s.\n", patt_name);
			arPattDeleteHandle(*pattHandle_p);
			return (FALSE);
		}

		return (TRUE);
	}

	static int loadMultiMarker(const char *patt_name, ARHandle *arHandle, ARPattHandle **pattHandle_p, ARMultiMarkerInfoT **arMultiConfig) {
		if( (*arMultiConfig = arMultiReadConfigFile(patt_name, *pattHandle_p)) == NULL ) {
			ARLOGe("config data load error !!\n");
			arPattDeleteHandle(*pattHandle_p);
			return (FALSE);
		}
		if( (*arMultiConfig)->patt_type == AR_MULTI_PATTERN_DETECTION_MODE_TEMPLATE ) {
			arSetPatternDetectionMode( arHandle, AR_TEMPLATE_MATCHING_COLOR );
		} else if( (*arMultiConfig)->patt_type == AR_MULTI_PATTERN_DETECTION_MODE_MATRIX ) {
			arSetPatternDetectionMode( arHandle, AR_MATRIX_CODE_DETECTION );
		} else { // AR_MULTI_PATTERN_DETECTION_MODE_TEMPLATE_AND_MATRIX
			arSetPatternDetectionMode( arHandle, AR_TEMPLATE_MATCHING_COLOR_AND_MATRIX );
		}

		return (TRUE);
	}


	int addMarker(int id, std::string patt_name) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		// const char *patt_name
		// Load marker(s).
		if (!loadMarker(patt_name.c_str(), &(arc->patt_id), arc->arhandle, &(arc->arPattHandle))) {
			ARLOGe("ARToolKitJS(): Unable to set up AR marker.\n");
			return -1;
		}

		return arc->patt_id;
	}

	int addMultiMarker(int id, std::string patt_name) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		// const char *patt_name
		// Load marker(s).
		if (!loadMultiMarker(patt_name.c_str(), arc->arhandle, &(arc->arPattHandle), &(arc->arMultiMarkerHandle))) {
			ARLOGe("ARToolKitJS(): Unable to set up AR multimarker.\n");
			return -1;
		}

		int multiMarker_id = arc->multi_markers.size();
		multi_marker marker = multi_marker();
		marker.id = multiMarker_id;
		marker.multiMarkerHandle = arc->arMultiMarkerHandle;

		arc->multi_markers.push_back(marker);

		return marker.id;
	}

	int getMultiMarkerNum(int id, int multiMarker_id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		int mId = multiMarker_id;
		if (mId < 0 || arc->multi_markers.size() <= mId) {
			return -1;
		}
		return (arc->multi_markers[mId].multiMarkerHandle)->marker_num;
	}

	int getMultiMarkerCount(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		return arc->multi_markers.size();
	}



	/**********************
	* Setters and getters *
	**********************/

	void setProjectionNearPlane(int id, const ARdouble projectionNearPlane) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);
		arc->nearPlane = projectionNearPlane;
	}

	ARdouble getProjectionNearPlane(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);
		return arc->nearPlane;
	}

	void setProjectionFarPlane(int id, const ARdouble projectionFarPlane) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);
		arc->farPlane = projectionFarPlane;
	}

	ARdouble getProjectionFarPlane(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);
		return arc->farPlane;
	}

	void setPatternDetectionMode(int id, int mode) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);
		if (arSetPatternDetectionMode(arc->arhandle, mode) == 0) {
			ARLOGi("Pattern detection mode set to %d.\n", mode);
		}
	}

	int getPatternDetectionMode(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		int mode;
		arController *arc = &(arControllers[id]);
		if (arGetPatternDetectionMode(arc->arhandle, &mode) == 0) {
			return mode;
		}

		return -1;
	}

	void setPattRatio(int id, float ratio) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);

		if (ratio <= 0.0f || ratio >= 1.0f) return;
		ARdouble pattRatio = (ARdouble)ratio;
		if (arc->arhandle) {
			if (arSetPattRatio(arc->arhandle, pattRatio) == 0) {
				ARLOGi("Pattern ratio size set to %f.\n", pattRatio);
			}
		}
	}

	ARdouble getPattRatio(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		ARdouble pattRatio;
		if (arc->arhandle) {
			if (arGetPattRatio(arc->arhandle, &pattRatio) == 0) {
				return pattRatio;
			}
		}

		return -1;
	}

	void setMatrixCodeType(int id, int type) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);

		AR_MATRIX_CODE_TYPE matrixType = (AR_MATRIX_CODE_TYPE)type;
		arSetMatrixCodeType(arc->arhandle, matrixType);
	}

	int getMatrixCodeType(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		AR_MATRIX_CODE_TYPE matrixType;
		arGetMatrixCodeType(arc->arhandle, &matrixType);
		return matrixType;
	}

	void setLabelingMode(int id, int mode) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);

		int labelingMode = mode;

		if (arSetLabelingMode(arc->arhandle, labelingMode) == 0) {
			ARLOGi("Labeling mode set to %d\n", labelingMode);
		}
	}

	int getLabelingMode(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		int labelingMode;

		if (arGetLabelingMode(arc->arhandle, &labelingMode) == 0) {
			return labelingMode;
		}

		return -1;
	}

	void setThreshold(int id, int threshold) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);

		if (threshold < 0 || threshold > 255) return;
		if (arSetLabelingThresh(arc->arhandle, threshold) == 0) {
			ARLOGi("Threshold set to %d\n", threshold);
		};
		// default 100
		// arSetLabelingThreshMode
		// AR_LABELING_THRESH_MODE_MANUAL, AR_LABELING_THRESH_MODE_AUTO_MEDIAN, AR_LABELING_THRESH_MODE_AUTO_OTSU, AR_LABELING_THRESH_MODE_AUTO_ADAPTIVE
	}

	int getThreshold(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		int threshold;
		if (arGetLabelingThresh(arc->arhandle, &threshold) == 0) {
			return threshold;
		};

		return -1;
	}

	void setThresholdMode(int id, int mode) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);

		AR_LABELING_THRESH_MODE thresholdMode = (AR_LABELING_THRESH_MODE)mode;

		if (arSetLabelingThreshMode(arc->arhandle, thresholdMode) == 0) {
			ARLOGi("Threshold mode set to %d\n", (int)thresholdMode);
		}
	}

	int getThresholdMode(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		AR_LABELING_THRESH_MODE thresholdMode;

		if (arGetLabelingThreshMode(arc->arhandle, &thresholdMode) == 0) {
			return thresholdMode;
		}

		return -1;
	}

	int setDebugMode(int id, int enable) {
		if (arControllers.find(id) == arControllers.end()) { return NULL; }
		arController *arc = &(arControllers[id]);

		arSetDebugMode(arc->arhandle, enable ? AR_DEBUG_ENABLE : AR_DEBUG_DISABLE);
		ARLOGi("Debug mode set to %s\n", enable ? "on." : "off.");

		return enable;
	}

	int getProcessingImage(int id) {
		if (arControllers.find(id) == arControllers.end()) { return NULL; }
		arController *arc = &(arControllers[id]);

		return (int)arc->arhandle->labelInfo.bwImage;
	}

	int getDebugMode(int id) {
		if (arControllers.find(id) == arControllers.end()) { return NULL; }
		arController *arc = &(arControllers[id]);

		int enable;

		arGetDebugMode(arc->arhandle, &enable);
		return enable;
	}

	void setImageProcMode(int id, int mode) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);

		int imageProcMode = mode;
		if (arSetImageProcMode(arc->arhandle, mode) == 0) {
			ARLOGi("Image proc. mode set to %d.\n", imageProcMode);
		}
	}

	int getImageProcMode(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		int imageProcMode;
		if (arGetImageProcMode(arc->arhandle, &imageProcMode) == 0) {
			return imageProcMode;
		}

		return -1;
	}




	/*
	 * Marker processing
	 */

	void matrixCopy(ARdouble src[3][4], ARdouble dst[3][4]) {
		for (int i=0; i<3; i++) {
			for (int j=0; j<4; j++) {
				dst[i][j] = src[i][j];
			}
		}
	}

	int getTransMatSquare(int id, int markerIndex, int markerWidth) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->arhandle->marker_num <= markerIndex) {
			return MARKER_INDEX_OUT_OF_BOUNDS;
		}
		ARMarkerInfo* marker = markerIndex < 0 ? &gMarkerInfo : &((arc->arhandle)->markerInfo[markerIndex]);

		arGetTransMatSquare(arc->ar3DHandle, marker, markerWidth, gTransform);

		return 0;
	}

	int getTransMatSquareCont(int id, int markerIndex, int markerWidth) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->arhandle->marker_num <= markerIndex) {
			return MARKER_INDEX_OUT_OF_BOUNDS;
		}
		ARMarkerInfo* marker = markerIndex < 0 ? &gMarkerInfo : &((arc->arhandle)->markerInfo[markerIndex]);

		arGetTransMatSquareCont(arc->ar3DHandle, marker, gTransform, markerWidth, gTransform);

		return 0;
	}

	int setMarkerInfoDir(int id, int markerIndex, int dir) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->arhandle->marker_num <= markerIndex) {
			return MARKER_INDEX_OUT_OF_BOUNDS;
		}
		ARMarkerInfo* marker = markerIndex < 0 ? &gMarkerInfo : &((arc->arhandle)->markerInfo[markerIndex]);

		marker->dir = dir;
		
		return 0;
	}

	int setMarkerInfoVertex(int id, int markerIndex) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->arhandle->marker_num <= markerIndex) {
			return MARKER_INDEX_OUT_OF_BOUNDS;
		}
		ARMarkerInfo* marker = markerIndex < 0 ? &gMarkerInfo : &((arc->arhandle)->markerInfo[markerIndex]);

		auto v = marker->vertex;

		v[0][0] = gTransform[0][0];
		v[0][1] = gTransform[0][1];
		v[1][0] = gTransform[0][2];
		v[1][1] = gTransform[0][3];
		v[2][0] = gTransform[1][0];
		v[2][1] = gTransform[1][1];
		v[3][0] = gTransform[1][2];
		v[3][1] = gTransform[1][3];

		marker->pos[0] = (v[0][0] + v[1][0] + v[2][0] + v[3][0]) * 0.25;
		marker->pos[1] = (v[0][1] + v[1][1] + v[2][1] + v[3][1]) * 0.25;

		return 0;
	}

	int getTransMatMultiSquareRobust(int id, int multiMarkerId) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->multi_markers.size() <= multiMarkerId || multiMarkerId < 0) {
			return MULTIMARKER_NOT_FOUND;
		}
		multi_marker *multiMatch = &(arc->multi_markers[multiMarkerId]);
		ARMultiMarkerInfoT *arMulti = multiMatch->multiMarkerHandle;

		arGetTransMatMultiSquareRobust( arc->ar3DHandle, arc->arhandle->markerInfo, arc->arhandle->marker_num, arMulti );
		matrixCopy(arMulti->trans, gTransform);

		return 0;
	}

	int getTransMatMultiSquare(int id, int multiMarkerId) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->multi_markers.size() <= multiMarkerId || multiMarkerId < 0) {
			return MULTIMARKER_NOT_FOUND;
		}
		multi_marker *multiMatch = &(arc->multi_markers[multiMarkerId]);
		ARMultiMarkerInfoT *arMulti = multiMatch->multiMarkerHandle;

		arGetTransMatMultiSquare( arc->ar3DHandle, arc->arhandle->markerInfo, arc->arhandle->marker_num, arMulti );
		matrixCopy(arMulti->trans, gTransform);

		return 0;
	}

	int detectMarker(int id) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

        // Convert video frame to AR2VideoBufferT
        AR2VideoBufferT buff = {0};
        buff.buff = arc->videoFrame;
        buff.fillFlag = 1;

        if (arc->pixFormat == AR_PIXEL_FORMAT_MONO) {
            buff.buffLuma = buff.buff;
        } else {
            if (!arc->lumaInfo) {
                arc->lumaInfo = arVideoLumaInit(arc->width, arc->height, arc->pixFormat);
                if (!arc->lumaInfo) {
                    ARLOGe("Error: unable to initialise luma conversion.\n");
                    exit(-1);
                }
            }
            buff.buffLuma = arVideoLuma(arc->lumaInfo, buff.buff);
        }

		return arDetectMarker( arc->arhandle, &buff);
	}
    

	int getMarkerNum(int id) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		return arc->arhandle->marker_num;
	}

	int getMultiEachMarkerInfo(int id, int multiMarkerId, int markerIndex) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->multi_markers.size() <= multiMarkerId || multiMarkerId < 0) {
			return MULTIMARKER_NOT_FOUND;
		}
		multi_marker *multiMatch = &(arc->multi_markers[multiMarkerId]);
		ARMultiMarkerInfoT *arMulti = multiMatch->multiMarkerHandle;

		if (arMulti->marker_num <= markerIndex || markerIndex < 0) {
			return MARKER_INDEX_OUT_OF_BOUNDS;
		}

		ARMultiEachMarkerInfoT *marker = &(arMulti->marker[markerIndex]);
		matrixCopy(marker->trans, gTransform);

		EM_ASM_({
			if (!artoolkit["multiEachMarkerInfo"]) {
				artoolkit["multiEachMarkerInfo"] = ({});
			}
			var multiEachMarker = artoolkit["multiEachMarkerInfo"];
			multiEachMarker['visible'] = $0;
			multiEachMarker['pattId'] = $1;
			multiEachMarker['pattType'] = $2;
			multiEachMarker['width'] = $3;
		},
			marker->visible,
			marker->patt_id,
			marker->patt_type,
			marker->width
		);

		return 0;
	}

	int getMarkerInfo(int id, int markerIndex) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->arhandle->marker_num <= markerIndex) {
			return MARKER_INDEX_OUT_OF_BOUNDS;
		}
		ARMarkerInfo* markerInfo = markerIndex < 0 ? &gMarkerInfo : &((arc->arhandle)->markerInfo[markerIndex]);

		EM_ASM_({
			var $a = arguments;
			var i = 12;
			if (!artoolkit["markerInfo"]) {
				artoolkit["markerInfo"] = ({
					pos: [0,0],
					line: [[0,0,0], [0,0,0], [0,0,0], [0,0,0]],
					vertex: [[0,0], [0,0], [0,0], [0,0]]
				});
			}
			var markerInfo = artoolkit["markerInfo"];
			markerInfo["area"] = $0;
			markerInfo["id"] = $1;
			markerInfo["idPatt"] = $2;
			markerInfo["idMatrix"] = $3;
			markerInfo["dir"] = $4;
			markerInfo["dirPatt"] = $5;
			markerInfo["dirMatrix"] = $6;
			markerInfo["cf"] = $7;
			markerInfo["cfPatt"] = $8;
			markerInfo["cfMatrix"] = $9;
			markerInfo["pos"][0] = $10;
			markerInfo["pos"][1] = $11;
			markerInfo["line"][0][0] = $a[i++];
			markerInfo["line"][0][1] = $a[i++];
			markerInfo["line"][0][2] = $a[i++];
			markerInfo["line"][1][0] = $a[i++];
			markerInfo["line"][1][1] = $a[i++];
			markerInfo["line"][1][2] = $a[i++];
			markerInfo["line"][2][0] = $a[i++];
			markerInfo["line"][2][1] = $a[i++];
			markerInfo["line"][2][2] = $a[i++];
			markerInfo["line"][3][0] = $a[i++];
			markerInfo["line"][3][1] = $a[i++];
			markerInfo["line"][3][2] = $a[i++];
			markerInfo["vertex"][0][0] = $a[i++];
			markerInfo["vertex"][0][1] = $a[i++];
			markerInfo["vertex"][1][0] = $a[i++];
			markerInfo["vertex"][1][1] = $a[i++];
			markerInfo["vertex"][2][0] = $a[i++];
			markerInfo["vertex"][2][1] = $a[i++];
			markerInfo["vertex"][3][0] = $a[i++];
			markerInfo["vertex"][3][1] = $a[i++];
			markerInfo["errorCorrected"] = $a[i++];
			// markerInfo["globalID"] = $a[i++];
		},
			markerInfo->area,
			markerInfo->id,
			markerInfo->idPatt,
			markerInfo->idMatrix,
			markerInfo->dir,
			markerInfo->dirPatt,
			markerInfo->dirMatrix,
			markerInfo->cf,
			markerInfo->cfPatt,
			markerInfo->cfMatrix,

			markerInfo->pos[0],
			markerInfo->pos[1],

			markerInfo->line[0][0],
			markerInfo->line[0][1],
			markerInfo->line[0][2],

			markerInfo->line[1][0],
			markerInfo->line[1][1],
			markerInfo->line[1][2],

			markerInfo->line[2][0],
			markerInfo->line[2][1],
			markerInfo->line[2][2],

			markerInfo->line[3][0],
			markerInfo->line[3][1],
			markerInfo->line[3][2],

			//

			markerInfo->vertex[0][0],
			markerInfo->vertex[0][1],

			markerInfo->vertex[1][0],
			markerInfo->vertex[1][1],

			markerInfo->vertex[2][0],
			markerInfo->vertex[2][1],

			markerInfo->vertex[3][0],
			markerInfo->vertex[3][1],

			//

			markerInfo->errorCorrected

			// markerInfo->globalID
		);

		return 0;
	}


	/********
	* Setup *
	********/

	int setup(int width, int height, int cameraID) {
		int id = gARControllerID++;
		arController *arc = &(arControllers[id]);
		arc->id = id;

		arc->width = width;
		arc->height = height;

		arc->videoFrameSize = width * height * 4 * sizeof(ARUint8);
		arc->videoFrame = (ARUint8*) malloc(arc->videoFrameSize);

		if ((arc->arPattHandle = arPattCreateHandle()) == NULL) {
			ARLOGe("setup(): Error: arPattCreateHandle.\n");
		}

		setCamera(id, cameraID);

		ARLOGi("Allocated videoFrameSize %d\n", arc->videoFrameSize);

		EM_ASM_({
			if (!artoolkit["frameMalloc"]) {
				artoolkit["frameMalloc"] = ({});
			}
			var frameMalloc = artoolkit["frameMalloc"];
			frameMalloc["framepointer"] = $1;
			frameMalloc["framesize"] = $2;
			frameMalloc["camera"] = $3;
			frameMalloc["transform"] = $4;
		},
			arc->id,
			arc->videoFrame,
			arc->videoFrameSize,
			arc->cameraLens,
			gTransform
		);


		return arc->id;
	}



}

#include "ARBindEM.cpp"