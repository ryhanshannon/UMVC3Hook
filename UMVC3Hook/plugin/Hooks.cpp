#include "Hooks.h"
#include "Menu.h"
#include "..\gui\notifications.h"
#include "..\rollback\FrameSync.h"

int64 CameraConstrutctor_Hook(int64 camera, int64 a2)
{
	char unk[72] = {};

	TheCamera = (UMVC3GameCamera*)camera;

	if (*(char*)(camera + 1416) == 1)
	{
		((void(__fastcall*)(int64))_addr(0x140015710))(camera + 0x230);
	}
	else
	{
		((void(__fastcall*)(int64*))_addr(0x14000BAA0))((int64*)&unk);
		((void(__fastcall*)(int64*, float, float, float, float))_addr(0x14045F7D0))((int64*)&unk, *(float*)(camera + 0x44), *(float*)(camera + 0x40), *(float*)(camera + 0x4C) * 0.017453292f, *(float*)(camera + 0x48));
		((void(__fastcall*)(int64, int64*))_addr(0x14000BA60))(a2, (int64*)&unk);

	}
	return a2;
}



void PluginProcess()
{
	// Wait for the game to fully initialize before installing hooks.
	// The research DLL uses Sleep(5000); we do the same to ensure
	// code at the input CALL site is decrypted and ready.
	Sleep(5000);

	if (umvc3::InstallFrameBoundaryHook())
		eLog::Message(__FUNCTION__, "INFO: Frame boundary hook installed");
	else
		eLog::Message(__FUNCTION__, "WARN: Frame boundary hook failed to install");

	while (true)
	{
		TheMenu->Process();
		Notifications->Update();
		Sleep(1);
	}
}
