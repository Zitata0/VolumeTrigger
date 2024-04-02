#include <Windows.h>
#include <string>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <Psapi.h>
#include <time.h>
#include <atlbase.h>

#pragma comment(lib, "version.lib")

//�������� HRESULT
void CHECK_HR(HRESULT hr) {
	if (hr != S_OK) {
		DWORD lastError = GetLastError();
		printf("Error: hr code - %d", lastError);
		/*
		switch (lastError) {
		case 0: {
			printf("[Divice not found]");
			break;
		}
		}
		*/
		exit(0);
	}
}

//������� ��������� ������ ��� EnumWindows()
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
	LPDWORD pid = new DWORD;
	DWORD tid = GetWindowThreadProcessId(hwnd, pid);
	if ((DWORD)std::atoi((*(std::string*)lParam).c_str()) == *pid) {
		char c[80];
		GetWindowTextA(hwnd, c, 80);
		if (GetWindow(hwnd, GW_OWNER) == (HWND)0 && IsWindowVisible(hwnd)) {
			if (c[0] != NULL) {
				*(std::string*)lParam = c;
				return false; //��������� �������������
			}
		}
	}
	delete pid;
	return true; //����������
}

//����� �������� ���� � ������� �� �������������� ��������
std::string getWindowName(LPARAM pid) {
	EnumWindows(EnumWindowsProc, pid);
	return "";
}

//��������� �������� �����
std::string getDescription(char* filePath) {

	UINT chars;
	//char filePath[] = "C:\\Users\\Admin\\AppData\\Local\\Yandex\\YandexBrowser\\Application\\browser.exe"; //���� �� ����� �������
	DWORD charsSize = GetFileVersionInfoSizeA(filePath, NULL); //�������� ���������� ��������
	if (charsSize == 0) return "Error: Information not found"; //����������������, ������ �������� ��-�� ������������� ����

	struct LANGANDCODEPAGE {
		WORD wLanguage;
		WORD wCodePage;
	} *lpTranslate = 0;

	UINT uiTranslate;
	LPVOID lpData = (LPVOID)malloc(charsSize); //�������� ������
	SecureZeroMemory(lpData, charsSize); //��������� ������ ������
	std::string result; //���������

	if (GetFileVersionInfoA(filePath, NULL, charsSize, lpData)) {

		VerQueryValueA(lpData, "\\VarFileInfo\\Translation", (LPVOID*)&lpTranslate, &uiTranslate);
		char strBlock[MAX_PATH] = { 0 };
		char* lpBuffer = nullptr;

		for (int i = 0; i < (uiTranslate / sizeof(struct LANGANDCODEPAGE)); i++)
		{
			sprintf_s(strBlock, "\\StringFileInfo\\%04x%04x\\FileDescription", lpTranslate[i].wLanguage, lpTranslate[i].wCodePage);
			VerQueryValueA(lpData, strBlock, (void**)&lpBuffer, &chars);
			for (int i = 0; i < chars; i++) result += lpBuffer[i];
		}
	}
	free(lpData);
	return result;
}

std::vector<DWORD> inactiveSessionList;

void updateSessionVolume(DWORD pid);

class AudioSessionEvents : public IAudioSessionEvents {
private:
	DWORD pid;
	LONG m_cRefAll = 1;
public:
	AudioSessionEvents(DWORD pid) {
		this->pid = pid;
	}

	HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(_In_  LPCWSTR NewDisplayName, LPCGUID EventContext) {
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnIconPathChanged(_In_  LPCWSTR NewIconPath, LPCGUID EventContext) {
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(_In_  float NewVolume, _In_  BOOL NewMute, LPCGUID EventContext) {
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(_In_  DWORD ChannelCount, _In_reads_(ChannelCount)  float NewChannelVolumeArray[], _In_  DWORD ChangedChannel, LPCGUID EventContext) {
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(_In_  LPCGUID NewGroupingParam, LPCGUID EventContext) {
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnStateChanged(_In_  AudioSessionState NewState) {
		if (NewState == AudioSessionStateInactive) {
			updateSessionVolume(pid);
		}else if (NewState == AudioSessionStateExpired) {
			inactiveSessionList.push_back(pid);
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason DisconnectReason) {
		return S_OK;
	}

	//IUnknown
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) {
		if (IID_IUnknown == riid)
		{
			AddRef();
			*ppv = (IUnknown*)this;
		}
		else if (__uuidof(IAudioSessionNotification) == riid)
		{
			AddRef();
			*ppv = (IAudioSessionNotification*)this;
		}
		else
		{
			*ppv = NULL;
			return E_NOINTERFACE;
		}
		return S_OK;
	}

	ULONG STDMETHODCALLTYPE AddRef(void) {
		return InterlockedIncrement(&m_cRefAll);
	}

	ULONG STDMETHODCALLTYPE Release(void) {
		ULONG ulRef = InterlockedDecrement(&m_cRefAll);
		if (0 == ulRef)
		{
			delete this;
		}
		return ulRef;
	}
};

class Session {
private:
	CComPtr<IAudioSessionControl2> audioSessionControl2; //�������� id �������� ������
	CComPtr<IAudioMeterInformation> audioMeterInformation; //�������� ��������� ������
	CComPtr<ISimpleAudioVolume> simpleAudioVolume; //�������� ��������� ��������� ������
	CComPtr<AudioSessionEvents> audioSessionEvents; //������� ������

	DWORD pid; //������������� �������� ������
	std::string name; //�������� ������
	float volume; //��������� ����� ������

	struct Normalize {
		bool enabled;
		time_t time;
		float changedVolume;
	} normalize;

	//������� �������� ���������� � ��������� �� ���� � ������
	void findName(DWORD pid) {
		if (pid == 0) {
			name = "System volume";
			return;
		}

		HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
		LPSTR lp = new char[MAX_PATH];
		SecureZeroMemory(lp, MAX_PATH);
		GetModuleFileNameExA(handle, NULL, lp, MAX_PATH);

		std::string appName = std::to_string(pid);
		getWindowName((LPARAM)&appName);

		if (appName.compare(std::to_string(pid))) {
			name = appName;
		}
		else {
			name = getDescription(lp);
		}

		delete[] lp;
	};
public:
	Session(IAudioSessionControl& audioSessionControl) {

		name = "";
		volume = 0;
		normalize = { 0 };
		
		CHECK_HR(audioSessionControl.QueryInterface(IID_PPV_ARGS(&audioSessionControl2)));
		CHECK_HR(audioSessionControl.QueryInterface(IID_PPV_ARGS(&audioMeterInformation)));
		CHECK_HR(audioSessionControl.QueryInterface(IID_PPV_ARGS(&simpleAudioVolume)));

		audioSessionControl2->GetProcessId(&pid); //��������� id ��������
		findName(pid);
		updateVolume();

		//����������� ������� ������
		audioSessionEvents = new AudioSessionEvents(pid);
		audioSessionControl2->RegisterAudioSessionNotification(audioSessionEvents);
	}

	~Session() {
		setVolume(volume);
		audioSessionControl2->UnregisterAudioSessionNotification(audioSessionEvents);
	}

	//���������� ������������
	void resetNormalize() {
		normalize = { 0 };
	}

	//���������� ������� ��������� ������
	float getVolumePeak() {
		float volume = 0;
		audioMeterInformation->GetPeakValue(&volume);
		return volume;
	}

	//�������� ��������� ��������� ������
	void setVolume(float volume) {
		if (volume < 0) {
			volume = 0;
		}
		simpleAudioVolume->SetMasterVolume(volume, 0);
	}

	//����������� �������� �����
	float getVolume() {
		return volume;
	}

	//��������� ���� ����� ������� ��������� ������
	void updateVolume() {
		simpleAudioVolume->GetMasterVolume(&volume);
	}

	//���������� �������� ������. ��������� ���� ���������� ��� �������� ����� �������.
	std::string getName() {
		return name;
	}

	//���������� �������� ���������������� �����
	float getNormalizedVolume() {
		return (getVolumePeak() / volume * (volume - normalize.changedVolume));
	}

	//����������� ���� �������
	void normalizeChannelVolume() {
		time_t currentTime = time(0);
		int timeStep = 1;


	}

	//����������� ���� ������
	void normalizeVolume() {

		time_t currentTime = time(0);
		int timeStep = 1;

		/*
		* �������� �� ����� ������ �����
		if (currentTime - normalize.time < timeStep) {
			return;
		}*/

		float volumeStep = volume / 20;

		if (getNormalizedVolume() > 0.3) {

			if (!normalize.enabled) {
				updateVolume();
			}

			if (volume - (normalize.changedVolume + volumeStep) < 0) {
				return;
			}

			normalize.enabled = true;
			normalize.time = currentTime;
			normalize.changedVolume += volumeStep;
			setVolume(volume - normalize.changedVolume);

			return;
		}

		if (!normalize.enabled || (currentTime - normalize.time) < timeStep) {
			return;
		}

		if (normalize.changedVolume > volumeStep) {
			normalize.time = currentTime;
			normalize.changedVolume -= volumeStep;
		}
		else {
			resetNormalize();
		}
		setVolume(volume - normalize.changedVolume);
	}

	//���������� id ��������
	DWORD getPid() {
		return pid;
	}
};

std::vector<Session*> sessionList;

class AudioSessionNotification : public IAudioSessionNotification {
private:
	LONG m_cRefAll = 1;
public:
	HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* session) {
		if (session) {
			sessionList.push_back(new Session(*session));
		}
		return S_OK;
	}

	//IUnknown
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) {
		if (IID_IUnknown == riid)
		{
			AddRef();
			*ppv = (IUnknown*)this;
		}
		else if (__uuidof(IAudioSessionNotification) == riid)
		{
			AddRef();
			*ppv = (IAudioSessionNotification*)this;
		}
		else
		{
			*ppv = NULL;
			return E_NOINTERFACE;
		}
		return S_OK;
	}

	ULONG STDMETHODCALLTYPE AddRef(void) {
		return InterlockedIncrement(&m_cRefAll);
	}

	ULONG STDMETHODCALLTYPE Release(void) {
		ULONG ulRef = InterlockedDecrement(&m_cRefAll);
		if (0 == ulRef)
		{
			delete this;
		}
		return ulRef;
	}
};

void updateSessionVolume(DWORD pid) {
	for (Session* session : sessionList) {
		if (session->getPid() == pid) {
			session->setVolume(session->getVolume());
			session->resetNormalize();
		}
	}
}
