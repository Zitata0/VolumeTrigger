#include <Windows.h>
#include <string>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <Psapi.h>
#include <time.h>
#include <atlbase.h>

#pragma comment(lib, "version.lib")

//Проверка HRESULT
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

//Функция обратного вызова для EnumWindows()
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
	LPDWORD pid = new DWORD;
	DWORD tid = GetWindowThreadProcessId(hwnd, pid);
	if ((DWORD)std::atoi((*(std::string*)lParam).c_str()) == *pid) {
		char c[80];
		GetWindowTextA(hwnd, c, 80);
		if (GetWindow(hwnd, GW_OWNER) == (HWND)0 && IsWindowVisible(hwnd)) {
			if (c[0] != NULL) {
				*(std::string*)lParam = c;
				return false; //Остановка перечислителя
			}
		}
	}
	delete pid;
	return true; //Продолжить
}

//Вывод названия окна в консоль по идентификатору процесса
std::string getWindowName(LPARAM pid) {
	EnumWindows(EnumWindowsProc, pid);
	return "";
}

//Получение описания файла
std::string getDescription(char* filePath) {

	UINT chars;
	//char filePath[] = "C:\\Users\\Admin\\AppData\\Local\\Yandex\\YandexBrowser\\Application\\browser.exe"; //Путь до файла запуска
	DWORD charsSize = GetFileVersionInfoSizeA(filePath, NULL); //Получить количество символов
	if (charsSize == 0) return "Error: Information not found"; //Предположительно, ошибка возможна из-за неправильного пути

	struct LANGANDCODEPAGE {
		WORD wLanguage;
		WORD wCodePage;
	} *lpTranslate = 0;

	UINT uiTranslate;
	LPVOID lpData = (LPVOID)malloc(charsSize); //Выделить память
	SecureZeroMemory(lpData, charsSize); //Заполнить память нулями
	std::string result; //Результат

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
	CComPtr<IAudioSessionControl2> audioSessionControl2; //Получить id процесса сессии
	CComPtr<IAudioMeterInformation> audioMeterInformation; //Получать громкость сессии
	CComPtr<ISimpleAudioVolume> simpleAudioVolume; //Изменять настройки громкости сессии
	CComPtr<AudioSessionEvents> audioSessionEvents; //События сессии

	DWORD pid; //Идентификатор процесса сессии
	std::string name; //Название сессии
	float volume; //Насйтрока звука сессии

	struct Normalize {
		bool enabled;
		time_t time;
		float changedVolume;
	} normalize;

	//Находит название приложения и заполняет им поле с именем
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

		audioSessionControl2->GetProcessId(&pid); //Получение id процесса
		findName(pid);
		updateVolume();

		//Регистрация событый сессии
		audioSessionEvents = new AudioSessionEvents(pid);
		audioSessionControl2->RegisterAudioSessionNotification(audioSessionEvents);
	}

	~Session() {
		setVolume(volume);
		audioSessionControl2->UnregisterAudioSessionNotification(audioSessionEvents);
	}

	//Сбрасывает нормализацию
	void resetNormalize() {
		normalize = { 0 };
	}

	//Возвращает текущую громкость сессии
	float getVolumePeak() {
		float volume = 0;
		audioMeterInformation->GetPeakValue(&volume);
		return volume;
	}

	//Изменяет настройку громкости сессии
	void setVolume(float volume) {
		if (volume < 0) {
			volume = 0;
		}
		simpleAudioVolume->SetMasterVolume(volume, 0);
	}

	//Возваращает значение звука
	float getVolume() {
		return volume;
	}

	//Обновляет поле звука текущем значением сессии
	void updateVolume() {
		simpleAudioVolume->GetMasterVolume(&volume);
	}

	//Возвращает название сессии. Заголовок окна приложения или описание файла запуска.
	std::string getName() {
		return name;
	}

	//Возвращает значение нормализованного звука
	float getNormalizedVolume() {
		return (getVolumePeak() / volume * (volume - normalize.changedVolume));
	}

	//Нормализует звук каналов
	void normalizeChannelVolume() {
		time_t currentTime = time(0);
		int timeStep = 1;


	}

	//Нормализует звук сессии
	void normalizeVolume() {

		time_t currentTime = time(0);
		int timeStep = 1;

		/*
		* Задержка на спуск уровня звука
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

	//Возвращает id процесса
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
