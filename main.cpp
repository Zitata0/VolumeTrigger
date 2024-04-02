#include <iostream>
#include <mmdeviceapi.h>
#include <vector>
#include <audiopolicy.h>

#include <list>

#include "Session.h"

void main() {

	setlocale(LC_ALL, "ru");

	CHECK_HR(CoInitializeEx(NULL, COINIT_MULTITHREADED));

	//Перечислитель устройств
	CComPtr<IMMDeviceEnumerator> deviceEnumerator;
	CHECK_HR(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (LPVOID*)&deviceEnumerator));

	//Устройство вывода
	CComPtr<IMMDevice> defaultDevice;
	CHECK_HR(deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice));
	deviceEnumerator.Release();

	//Менеджер аудиосеансов
	CComPtr<IAudioSessionManager2> audioSessionManager2;
	CHECK_HR(defaultDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, NULL, (LPVOID*)&audioSessionManager2));
	defaultDevice.Release();

	//Добавление новых аудиосессий по уведомлениям
	CComPtr<AudioSessionNotification> audioSessionNotification = new AudioSessionNotification();
	audioSessionManager2->RegisterSessionNotification(audioSessionNotification);

	//Перечислитель аудиосеансов
	CComPtr<IAudioSessionEnumerator> audioSessionEnumerator;
	CHECK_HR(audioSessionManager2->GetSessionEnumerator(&audioSessionEnumerator));

	//Количество аудиосессий
	int sessionCount = 0;
	audioSessionEnumerator->GetCount(&sessionCount);

	//Добавление аудиосессий в список
	for (int sessionIndex = 0; sessionIndex < sessionCount; sessionIndex++) {
		CComPtr<IAudioSessionControl> audioSessionControl;
		CHECK_HR(audioSessionEnumerator->GetSession(sessionIndex, &audioSessionControl));
		sessionList.push_back(new Session(*audioSessionControl));
	}

	while (true) {
		if (!inactiveSessionList.empty()) {
			for (int selectedSessionIndex = 0; selectedSessionIndex < sessionList.size(); selectedSessionIndex++) {
				for (int inactiveSessionIndex = 0; inactiveSessionIndex < inactiveSessionList.size(); inactiveSessionIndex++) {
					if (sessionList[selectedSessionIndex]->getPid() == inactiveSessionList[inactiveSessionIndex]) {
						delete sessionList[selectedSessionIndex];
						sessionList.erase(sessionList.begin() + selectedSessionIndex);
						selectedSessionIndex--;
						inactiveSessionList.erase(inactiveSessionList.begin() + inactiveSessionIndex);				
						inactiveSessionIndex--;
					}
				}
			}
		}

		for (Session* session : sessionList) {
			session->normalizeVolume();
			printf("%s - %d - %f = %f\n", session->getName().c_str(), session->getPid(), session->getVolumePeak(), session->getNormalizedVolume());
		}
		Sleep(100);
		system("cls");
	}

	audioSessionManager2->UnregisterSessionNotification(audioSessionNotification);
	CoUninitialize();
}