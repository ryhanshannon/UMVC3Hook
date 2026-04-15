#pragma once
#include "../umvc3/Vector.h"
#include "../umvc3/Matrix.h"
#include "../helper/eKeyboardMan.h"
#include "../utils.h"
#include "../gui/gui_impl_dx9.h"
#include "../rollback/StateSnapshot.h"
#include "../rollback/FrameSync.h"

#include <DirectXMath.h>
using namespace DirectX;

#define UMVC3HOOK_VERSION "0.2"

enum EMenuSubMenus {
	SM_Settings,
	SM_Total
};

class UMVC3Menu {
public:
	bool	 m_bIsActive = false;
	bool	 m_bSubmenuActive[SM_Total] = {};
	bool     m_bPressingKey = false;
	bool	 m_bIsFocused = false;
	bool	 m_bCustomCameraPos = false;
	bool	 m_bCustomCameraRot = false;
	bool	 m_bCustomCameraFOV = false;
	bool	 m_bFreeCam = false;
	bool	 m_bDisableHUD = false;
	bool	 m_bDisableHUDTotal = false;
	bool	 m_bDisableEffects = false;
	float	 m_fFreeCameraSpeed = 155.0f;
	float	 m_fFreeCameraRotationSpeed = 0.25f;

	int* m_pCurrentVarToChange = nullptr;

	// camera

	Matrix	 camMat = {};
	Vector   camRot = {};
	Vector   camPos = {};
	Vector   camTarget = {};
	float	 camFov = 0;


	UMVC3Menu();

	void	 OnActivate();
	void	 OnToggleFreeCamera();
	void	 OnToggleHUD();
	void	 Draw();
	void	 Process();
	void	 UpdateControls();
	void	 UpdateFreecam();

	void	 DrawCameraTab();
	void	 DrawMiscTab();
	void	 DrawRollbackTab();

	void	 DrawSettings();

	void	 DrawKeyBind(const char* name, int* var);
	void	 KeyBind(int* var, const char* bindName, const char* name);

	// Rollback test state
	uint64_t m_lastSaveMicros = 0;
	uint64_t m_lastLoadMicros = 0;
	uint64_t m_lastChecksum = 0;
	bool     m_saveRequested = false;
	bool     m_loadRequested = false;
	uint32_t m_requestedLoadFramesAgo = 0;
	int      m_manualLoadFramesAgo = 1;
	char     m_rollbackStatus[256] = {};
};


extern UMVC3Menu* TheMenu;
