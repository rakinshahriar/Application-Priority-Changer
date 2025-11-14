// Application Priority Changer.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "Application Priority Changer.h"

#include <string>
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <commctrl.h>
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "comctl32.lib")

#ifndef LVM_FIRST
#define LVM_FIRST 0x1000
#endif
#ifndef LVM_SETTOPINDEX
#define LVM_SETTOPINDEX (LVM_FIRST + 176)
#endif

// Ensure MAX_LOADSTRING is defined correctly
#ifndef MAX_LOADSTRING
#define MAX_LOADSTRING 100
#endif

// Control IDs (numeric values)
#define ID_EDIT_PROC 101
#define ID_COMBO_PRI 102
#define ID_BUTTON_APPLY 103
#define ID_LIST_PROCS 104
#define ID_BUTTON_USE 105
#define ID_EDIT_SEARCH 106
#define ID_BUTTON_REFRESH 107
#define ID_EDIT_INTERVAL 108

// Global Variables:
HINSTANCE hInst; // current instance
WCHAR szTitle[MAX_LOADSTRING]; // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING]; // the main window class name

// Sorting state
static int g_sortColumn =0; //0 = name,1 = pid,2 = priority
static bool g_sortAscending = true;

struct ProcessEntry {
 DWORD pid;
 std::wstring name;
 DWORD priorityClass; // numeric priority class value,0 if unknown
};

// Global runtime state (selection, timers, refresh)
static DWORD g_selectedPID = 0; // 0 means none
 static UINT g_refreshIntervalMs = 30000; // default 30s
 static bool g_isFullRefreshing = false;
 static const UINT_PTR TIMER_FULL_REFRESH = 1;
 static const UINT_PTR TIMER_PRIORITY_UPDATE = 2;

// avoid conflicts with Windows min/max macros by using local helpers
static inline int i_max(int a, int b) { return a > b ? a : b; }
static inline int i_min(int a, int b) { return a < b ? a : b; }

// Forward declarations
ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

// Helpers
static void RefreshProcessList(HWND hListView, HWND hSearchEdit);
static void ClearProcessEntries(HWND hListView);
static int CALLBACK ListViewCompare(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort);
static DWORD QueryProcessPriorityClass(DWORD pid);
static const wchar_t* PriorityClassToName(DWORD pclass);
static void AdjustListViewColumns(HWND hListView);

static void ClearProcessEntries(HWND hListView)
{
 int count = ListView_GetItemCount(hListView);
 for (int i =0; i < count; ++i) {
 LVITEMW lvi;
 ZeroMemory(&lvi, sizeof(lvi));
 lvi.iItem = i;
 lvi.mask = LVIF_PARAM;
 if (ListView_GetItem(hListView, &lvi)) {
 ProcessEntry* p = (ProcessEntry*)lvi.lParam;
 if (p) delete p;
 }
 }
 ListView_DeleteAllItems(hListView);
}

static int CALLBACK ListViewCompare(LPARAM lParam1, LPARAM lParam2, LPARAM /*lParamSort*/)
{
 ProcessEntry* a = (ProcessEntry*)lParam1;
 ProcessEntry* b = (ProcessEntry*)lParam2;
 if (!a || !b) return 0;

 int cmp =0;
 if (g_sortColumn ==0) {
 std::wstring an = a->name;
 std::wstring bn = b->name;
 std::transform(an.begin(), an.end(), an.begin(), ::towlower);
 std::transform(bn.begin(), bn.end(), bn.begin(), ::towlower);
 if (an < bn) cmp = -1;
 else if (an > bn) cmp =1;
 else cmp =0;
 } else if (g_sortColumn ==1) {
 if (a->pid < b->pid) cmp = -1;
 else if (a->pid > b->pid) cmp =1;
 else cmp =0;
 } else {
 if (a->priorityClass < b->priorityClass) cmp = -1;
 else if (a->priorityClass > b->priorityClass) cmp =1;
 else cmp =0;
 }

 return g_sortAscending ? cmp : -cmp;
}

static DWORD QueryProcessPriorityClass(DWORD pid)
{
 HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
 if (!h) {
 h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
 if (!h) return 0;
 }
 DWORD cls = GetPriorityClass(h);
 CloseHandle(h);
 return cls;
}

static const wchar_t* PriorityClassToName(DWORD pclass)
{
 switch (pclass) {
 case IDLE_PRIORITY_CLASS: return L"Idle";
 case BELOW_NORMAL_PRIORITY_CLASS: return L"BelowNormal";
 case NORMAL_PRIORITY_CLASS: return L"Normal";
 case ABOVE_NORMAL_PRIORITY_CLASS: return L"AboveNormal";
 case HIGH_PRIORITY_CLASS: return L"High";
 case REALTIME_PRIORITY_CLASS: return L"RealTime";
 default: return L"Unknown";
 }
}

// Update priorities only (no list re-population)
static void UpdatePriorities(HWND hListView)
{
 if (!hListView) return;
 int count = ListView_GetItemCount(hListView);
 for (int i =0; i < count; ++i)
 {
 LVITEMW lvi;
 ZeroMemory(&lvi, sizeof(lvi));
 lvi.iItem = i;
 lvi.mask = LVIF_PARAM;
 if (!ListView_GetItem(hListView, &lvi)) continue;
 ProcessEntry* p = (ProcessEntry*)lvi.lParam;
 if (!p) continue;
 DWORD pclass = QueryProcessPriorityClass(p->pid);
 wchar_t pribuf[64];
 const wchar_t* pname = PriorityClassToName(pclass);
 if (pclass !=0 && wcscmp(pname, L"Unknown") !=0)
 swprintf_s(pribuf, L"%s (%u)", pname, pclass);
 else if (pclass !=0)
 swprintf_s(pribuf, L"%u", pclass);
 else
 swprintf_s(pribuf, L"N/A");
 // Update only the priority column (index2)
 ListView_SetItemText(hListView, i,2, pribuf);
 // Also update the stored priorityClass value
 p->priorityClass = pclass;
 }
}

// RefreshProcessList: preserves top visible index and selection, restores top after refill
static void RefreshProcessList(HWND hListView, HWND hSearchEdit)
{
 g_isFullRefreshing = true;
 // get search text and lowercase it
 wchar_t searchBuf[260] = {0};
 GetWindowTextW(hSearchEdit, searchBuf, _countof(searchBuf));
 std::wstring search(searchBuf);
 std::transform(search.begin(), search.end(), search.begin(), ::towlower);

 DWORD prevSelectedPID = g_selectedPID;

 // record PID of top visible item so we can restore the same scroll position after sorting/filtering
 int topIndex = ListView_GetTopIndex(hListView);
 DWORD topPID =0;
 if (topIndex >=0)
 {
 LVITEMW tlv; ZeroMemory(&tlv, sizeof(tlv)); tlv.iItem = topIndex; tlv.mask = LVIF_PARAM;
 if (ListView_GetItem(hListView, &tlv)) {
 ProcessEntry* tp = (ProcessEntry*)tlv.lParam;
 if (tp) topPID = tp->pid;
 }
 }

 // clear and repopulate
 ClearProcessEntries(hListView);

 HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
 if (snap == INVALID_HANDLE_VALUE) { g_isFullRefreshing = false; return; }

 PROCESSENTRY32W pe; ZeroMemory(&pe, sizeof(pe)); pe.dwSize = sizeof(pe);
 if (Process32FirstW(snap, &pe)) {
 do {
 std::wstring name(pe.szExeFile);
 std::wstring lowName = name;
 std::transform(lowName.begin(), lowName.end(), lowName.begin(), ::towlower);

 if (search.empty() || lowName.find(search) != std::wstring::npos) {
 DWORD pclass = QueryProcessPriorityClass(pe.th32ProcessID);
 ProcessEntry* p = new ProcessEntry();
 p->pid = pe.th32ProcessID;
 p->name = name;
 p->priorityClass = pclass;

 LVITEMW lvi; ZeroMemory(&lvi, sizeof(lvi));
 lvi.mask = LVIF_TEXT | LVIF_PARAM;
 lvi.pszText = const_cast<LPWSTR>(p->name.c_str());
 lvi.iItem = ListView_GetItemCount(hListView);
 lvi.lParam = (LPARAM)p;
 int index = ListView_InsertItem(hListView, &lvi);

 wchar_t pidbuf[32]; swprintf_s(pidbuf, L"%u", p->pid);
 ListView_SetItemText(hListView, index,1, pidbuf);

 wchar_t pribuf[64];
 const wchar_t* pname = PriorityClassToName(pclass);
 if (pclass !=0 && wcscmp(pname, L"Unknown") !=0)
 swprintf_s(pribuf, L"%s (%u)", pname, pclass);
 else if (pclass !=0)
 swprintf_s(pribuf, L"%u", pclass);
 else
 swprintf_s(pribuf, L"N/A");
 ListView_SetItemText(hListView, index,2, pribuf);

 if (p->pid == prevSelectedPID) {
 // restore selection but don't set focus to avoid scrolling
 ListView_SetItemState(hListView, index, LVIS_SELECTED, LVIS_SELECTED);
 }
 }

 } while (Process32NextW(snap, &pe));
 }
 CloseHandle(snap);

 // apply sorting
 ListView_SortItems(hListView, ListViewCompare,0);

 // restore top visible item by PID if we recorded one (robust across sorting/filtering)
 if (topPID != 0) {
 int count = ListView_GetItemCount(hListView);
 for (int i = 0; i < count; ++i) {
 LVITEMW lvi; ZeroMemory(&lvi, sizeof(lvi)); lvi.iItem = i; lvi.mask = LVIF_PARAM;
 if (ListView_GetItem(hListView, &lvi)) {
 ProcessEntry* p = (ProcessEntry*)lvi.lParam;
 if (p && p->pid == topPID) {
 // Ensure the recorded item is visible after refresh. Using EnsureVisible
 // avoids relying on LVM_SETTOPINDEX which may not be available in all headers.
 ListView_EnsureVisible(hListView, i, FALSE);
 break;
 }
 }
 }
 } else {
 // Fallback: clamp previous topIndex if it exists
 int itemCount = ListView_GetItemCount(hListView);
 if (itemCount > 0 && topIndex > 0) {
 if (topIndex >= itemCount) topIndex = itemCount - 1;
 ListView_EnsureVisible(hListView, topIndex, FALSE);
 }
 }

 g_isFullRefreshing = false;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
 _In_opt_ HINSTANCE hPrevInstance,
 _In_ LPWSTR lpCmdLine,
 _In_ int nCmdShow)
{
 UNREFERENCED_PARAMETER(hPrevInstance);
 UNREFERENCED_PARAMETER(lpCmdLine);

 LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
 LoadStringW(hInstance, IDC_APPLICATIONPRIORITYCHANGER, szWindowClass, MAX_LOADSTRING);
 MyRegisterClass(hInstance);

 if (!InitInstance(hInstance, nCmdShow)) return FALSE;

 HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_APPLICATIONPRIORITYCHANGER));

 MSG msg;
 while (GetMessage(&msg, nullptr,0,0)) {
 if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) {
 TranslateMessage(&msg);
 DispatchMessage(&msg);
 }
 }

 return (int)msg.wParam;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
 WNDCLASSEXW wcex; ZeroMemory(&wcex, sizeof(wcex));
 wcex.cbSize = sizeof(WNDCLASSEX);
 wcex.style = CS_HREDRAW | CS_VREDRAW;
 wcex.lpfnWndProc = WndProc;
 wcex.hInstance = hInstance;
 wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPLICATIONPRIORITYCHANGER));
 wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
 wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW +1);
 wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_APPLICATIONPRIORITYCHANGER);
 wcex.lpszClassName = szWindowClass;
 wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));
 return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
 hInst = hInstance;

 INITCOMMONCONTROLSEX icex; ZeroMemory(&icex, sizeof(icex));
 icex.dwSize = sizeof(icex);
 icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
 InitCommonControlsEx(&icex);

 HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
 CW_USEDEFAULT,0,720,420, nullptr, nullptr, hInstance, nullptr);
 if (!hWnd) return FALSE;

 ShowWindow(hWnd, nCmdShow);
 UpdateWindow(hWnd);
 return TRUE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
 static HWND hEditProc = NULL;
 static HWND hComboPri = NULL;
 static HWND hListProcs = NULL;
 static HWND hEditSearch = NULL;
 static HWND hEditInterval = NULL;

 switch (message) {
 case WM_CREATE: {
 CreateWindowW(L"STATIC", L"Process (selected or type name):", WS_CHILD | WS_VISIBLE,
10,10,300,20, hWnd, NULL, hInst, NULL);

 hEditProc = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT,
10,30,300,24, hWnd, (HMENU)ID_EDIT_PROC, hInst, NULL);

 CreateWindowW(L"STATIC", L"Priority:", WS_CHILD | WS_VISIBLE,
320,10,80,20, hWnd, NULL, hInst, NULL);

 hComboPri = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
320,30,240,200, hWnd, (HMENU)ID_COMBO_PRI, hInst, NULL);

 int idx = (int)SendMessageW(hComboPri, CB_ADDSTRING,0, (LPARAM)L"Idle");
 SendMessageW(hComboPri, CB_SETITEMDATA, idx, (LPARAM)IDLE_PRIORITY_CLASS);
 idx = (int)SendMessageW(hComboPri, CB_ADDSTRING,0, (LPARAM)L"BelowNormal");
 SendMessageW(hComboPri, CB_SETITEMDATA, idx, (LPARAM)BELOW_NORMAL_PRIORITY_CLASS);
 idx = (int)SendMessageW(hComboPri, CB_ADDSTRING,0, (LPARAM)L"Normal");
 SendMessageW(hComboPri, CB_SETITEMDATA, idx, (LPARAM)NORMAL_PRIORITY_CLASS);
 idx = (int)SendMessageW(hComboPri, CB_ADDSTRING,0, (LPARAM)L"AboveNormal");
 SendMessageW(hComboPri, CB_SETITEMDATA, idx, (LPARAM)ABOVE_NORMAL_PRIORITY_CLASS);
 idx = (int)SendMessageW(hComboPri, CB_ADDSTRING,0, (LPARAM)L"High");
 SendMessageW(hComboPri, CB_SETITEMDATA, idx, (LPARAM)HIGH_PRIORITY_CLASS);
 idx = (int)SendMessageW(hComboPri, CB_ADDSTRING,0, (LPARAM)L"RealTime");
 SendMessageW(hComboPri, CB_SETITEMDATA, idx, (LPARAM)REALTIME_PRIORITY_CLASS);

 SendMessageW(hComboPri, CB_SETCURSEL, (WPARAM)2,0); // Normal

 CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
570,30,80,24, hWnd, (HMENU)ID_BUTTON_APPLY, hInst, NULL);

 CreateWindowW(L"STATIC", L"Search running processes:", WS_CHILD | WS_VISIBLE,
10,65,200,20, hWnd, NULL, hInst, NULL);
 hEditSearch = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT,
10,85,300,24, hWnd, (HMENU)ID_EDIT_SEARCH, hInst, NULL);

 CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
320,85,70,24, hWnd, (HMENU)ID_BUTTON_REFRESH, hInst, NULL);

 CreateWindowW(L"STATIC", L"Refresh interval (s):", WS_CHILD | WS_VISIBLE,
400,65,140,20, hWnd, NULL, hInst, NULL);
 hEditInterval = CreateWindowW(L"EDIT", L"30", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_NUMBER,
400,85,60,24, hWnd, (HMENU)ID_EDIT_INTERVAL, hInst, NULL);

 hListProcs = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
 WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
10,115,640,240, hWnd, (HMENU)ID_LIST_PROCS, hInst, NULL);

 ListView_SetExtendedListViewStyle(hListProcs, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_HEADERDRAGDROP);

 LVCOLUMNW col; ZeroMemory(&col, sizeof(col));
 col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
 col.cx =380; col.pszText = (LPWSTR)L"Name"; ListView_InsertColumn(hListProcs,0, &col);
 col.cx =100; col.pszText = (LPWSTR)L"PID"; ListView_InsertColumn(hListProcs,1, &col);
 col.cx =140; col.pszText = (LPWSTR)L"Priority"; ListView_InsertColumn(hListProcs,2, &col);

 // Initial proportional adjustment for columns
 AdjustListViewColumns(hListProcs);

 CreateWindowW(L"BUTTON", L"Use Selected", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
320,115,120,24, hWnd, (HMENU)ID_BUTTON_USE, hInst, NULL);

 RefreshProcessList(hListProcs, hEditSearch);

 // Full refresh timer (uses TIMER_FULL_REFRESH)
 SetTimer(hWnd, TIMER_FULL_REFRESH, g_refreshIntervalMs, NULL);
 // Priority-only update timer:2 seconds
 SetTimer(hWnd, TIMER_PRIORITY_UPDATE,2000, NULL);
 }
 break;

 case WM_COMMAND: {
 int id = LOWORD(wParam);
 int notif = HIWORD(wParam);

 if (id == ID_BUTTON_APPLY) {
 wchar_t procName[260] = {0 };
 GetWindowTextW((HWND)GetDlgItem(hWnd, ID_EDIT_PROC), procName, _countof(procName));
 if (wcslen(procName) ==0 && g_selectedPID ==0) {
 MessageBoxW(hWnd, L"Please provide a process name or select from the list.", L"Input required", MB_OK | MB_ICONEXCLAMATION);
 break;
 }

 HWND hCombo = (HWND)GetDlgItem(hWnd, ID_COMBO_PRI);
 LRESULT sel = SendMessageW(hCombo, CB_GETCURSEL,0,0);
 if (sel == CB_ERR) {
 MessageBoxW(hWnd, L"Please select a priority from the list.", L"Input required", MB_OK | MB_ICONEXCLAMATION);
 break;
 }
 LPARAM priVal = SendMessageW(hCombo, CB_GETITEMDATA, (WPARAM)sel,0);
 int priorityNumeric = (int)priVal;
 if (priorityNumeric == REALTIME_PRIORITY_CLASS) {
 int r = MessageBoxW(hWnd, L"RealTime priority can make the system unresponsive. Are you sure?", L"Warning", MB_YESNO | MB_ICONWARNING);
 if (r != IDYES) break;
 }

 std::wstring pri = std::to_wstring(priorityNumeric);
 std::wstring params;
 std::wstring proc(procName);
 bool havePid = (g_selectedPID != 0);
 bool haveName = (proc.size() > 0);

 if (havePid && haveName) {
 // Match by PID OR name (case-insensitive). Use a pipeline and Where-Object to combine conditions.
 // Example: Get-WmiObject Win32_Process | Where-Object { $_.ProcessId -eq 1234 -or $_.Name -ieq 'proc.exe' } | ForEach-Object { $_.SetPriority(8) }
 params = L"-Command \"Get-WmiObject Win32_Process | Where-Object { $_.ProcessId -eq " + std::to_wstring(g_selectedPID) + L" -or $_.Name -ieq \\\"" + proc + L"\\\" } | ForEach-Object { $_.SetPriority(" + pri + L") }\"";
 }
 else if (havePid) {
 params = L"-Command \"Get-WmiObject Win32_Process | Where-Object { $_.ProcessId -eq " + std::to_wstring(g_selectedPID) + L" } | ForEach-Object { $_.SetPriority(" + pri + L") }\"";
 }
 else if (haveName) {
 params = L"-Command \"Get-WmiObject Win32_Process | Where-Object { $_.Name -ieq \\\"" + proc + L"\\\" } | ForEach-Object { $_.SetPriority(" + pri + L") }\"";
 }
 else {
 MessageBoxW(hWnd, L"Please provide a process name or select from the list.", L"Input required", MB_OK | MB_ICONEXCLAMATION);
 break;
 }

 HINSTANCE result = ShellExecuteW(NULL, L"runas", L"powershell.exe", params.c_str(), NULL, SW_SHOWNORMAL);
 if ((INT_PTR)result <=32)
 MessageBoxW(hWnd, L"Failed to start PowerShell elevated.", L"Error", MB_OK | MB_ICONERROR);
 else
 MessageBoxW(hWnd, L"PowerShell started to change priority.", L"Info", MB_OK | MB_ICONINFORMATION);
 }
 else if (id == ID_BUTTON_REFRESH || (id == ID_EDIT_SEARCH && notif == EN_CHANGE)) {
 HWND hList = (HWND)GetDlgItem(hWnd, ID_LIST_PROCS);
 HWND hSearch = (HWND)GetDlgItem(hWnd, ID_EDIT_SEARCH);
 RefreshProcessList(hList, hSearch);
 }
 else if (id == ID_EDIT_INTERVAL && notif == EN_CHANGE) {
 wchar_t buf[64] = {0 };
 GetWindowTextW((HWND)GetDlgItem(hWnd, ID_EDIT_INTERVAL), buf, (int)_countof(buf));
 int seconds = _wtoi(buf);
 if (seconds <1) seconds =1;
 if (seconds >3600) seconds =3600;
 UINT newMs = (UINT)seconds *1000U;
 if (newMs != g_refreshIntervalMs) {
 g_refreshIntervalMs = newMs;
 KillTimer(hWnd, TIMER_FULL_REFRESH);
 SetTimer(hWnd, TIMER_FULL_REFRESH, g_refreshIntervalMs, NULL);
 }
 }
 else if (id == ID_BUTTON_USE) {
 HWND hList = (HWND)GetDlgItem(hWnd, ID_LIST_PROCS);
 int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
 if (sel != -1) {
 wchar_t buf[320];
 ListView_GetItemText(hList, sel,0, buf, (int)_countof(buf));
 SetWindowTextW((HWND)GetDlgItem(hWnd, ID_EDIT_PROC), buf);
 LVITEMW lvi; ZeroMemory(&lvi, sizeof(lvi)); lvi.iItem = sel; lvi.mask = LVIF_PARAM;
 if (ListView_GetItem(hList, &lvi)) {
 ProcessEntry* p = (ProcessEntry*)lvi.lParam;
 g_selectedPID = p ? p->pid :0;
 }
 }
 }
 else if (id == ID_EDIT_PROC && notif == EN_CHANGE) {
 g_selectedPID =0; // user typed name
 }
 else {
 switch (id) {
 case IDM_ABOUT:
 DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
 break;
 case IDM_EXIT:
 DestroyWindow(hWnd);
 break;
 default:
 return DefWindowProc(hWnd, message, wParam, lParam);
 }
 }
 }
 break;

 case WM_NOTIFY: {
 LPNMHDR pnm = (LPNMHDR)lParam;
 if (pnm->idFrom == ID_LIST_PROCS) {
 if (pnm->code == NM_DBLCLK) {
 HWND hList = (HWND)GetDlgItem(hWnd, ID_LIST_PROCS);
 int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
 if (sel != -1) {
 wchar_t buf[320];
 ListView_GetItemText(hList, sel,0, buf, (int)_countof(buf));
 SetWindowTextW((HWND)GetDlgItem(hWnd, ID_EDIT_PROC), buf);
 LVITEMW lvi; ZeroMemory(&lvi, sizeof(lvi)); lvi.iItem = sel; lvi.mask = LVIF_PARAM;
 if (ListView_GetItem(hList, &lvi)) {
 ProcessEntry* p = (ProcessEntry*)lvi.lParam;
 g_selectedPID = p ? p->pid :0;
 }
 }
 }
 else if (pnm->code == LVN_COLUMNCLICK) {
 LPNMLISTVIEW pnmv = (LPNMLISTVIEW)lParam;
 if (g_sortColumn == pnmv->iSubItem) g_sortAscending = !g_sortAscending;
 else { g_sortColumn = pnmv->iSubItem; g_sortAscending = true; }
 HWND hList = (HWND)GetDlgItem(hWnd, ID_LIST_PROCS);
 ListView_SortItems(hList, ListViewCompare,0);
 }
 }
 }
 break;

 case WM_TIMER: {
 HWND hList = (HWND)GetDlgItem(hWnd, ID_LIST_PROCS);
 HWND hSearch = (HWND)GetDlgItem(hWnd, ID_EDIT_SEARCH);
 if (wParam == TIMER_FULL_REFRESH)
 {
 if (hList && hSearch) RefreshProcessList(hList, hSearch);
 }
 else if (wParam == TIMER_PRIORITY_UPDATE)
 {
 // do not update priorities while a full refresh is in progress
 if (!g_isFullRefreshing && hList) UpdatePriorities(hList);
 }
 }
 break;
 
 case WM_SIZE: {
    // Reposition and resize controls so the listview expands/shrinks with the window.
    int newW = LOWORD(lParam);
    int newH = HIWORD(lParam);
    // enforce minimum sizes
    const int minW = 480;
    const int minH = 240;
    if (newW < minW) newW = minW;
    if (newH < minH) newH = minH;

    // Basic layout constants matching initial design
    const int margin = 10;
    const int controlHeight = 24;
    const int listTop = 115;
    const int bottomMargin = 20;

    // Move a few top controls (they'll clamp if window too small)
    HWND hEditProc = GetDlgItem(hWnd, ID_EDIT_PROC);
    if (hEditProc) MoveWindow(hEditProc, margin, 30, i_max(120, i_min(300, newW - 2*margin)), controlHeight, TRUE);
    HWND hComboPri = GetDlgItem(hWnd, ID_COMBO_PRI);
    if (hComboPri) MoveWindow(hComboPri, 320, 30, 240, controlHeight, TRUE);
    HWND hEditSearch = GetDlgItem(hWnd, ID_EDIT_SEARCH);
    if (hEditSearch) MoveWindow(hEditSearch, margin, 85, i_max(120, i_min(300, newW - 2*margin)), controlHeight, TRUE);
    HWND hEditInterval = GetDlgItem(hWnd, ID_EDIT_INTERVAL);
    if (hEditInterval) MoveWindow(hEditInterval, 400, 85, 60, controlHeight, TRUE);

    // Resize listview to fill available client area
    HWND hList = GetDlgItem(hWnd, ID_LIST_PROCS);
    if (hList) {
      int listW = i_max(200, newW - 2*margin);
      int listH = i_max(80, newH - listTop - bottomMargin);
      MoveWindow(hList, margin, listTop, listW, listH, TRUE);
      // Smoothly adjust all columns according to new size
      AdjustListViewColumns(hList);
     }
  }
  break;

 case WM_PAINT: {
 PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps); EndPaint(hWnd, &ps);
 }
 break;

 case WM_DESTROY: {
 // free any remaining entries
 HWND hList = (HWND)GetDlgItem(hWnd, ID_LIST_PROCS);
 if (hList) ClearProcessEntries(hList);
 KillTimer(hWnd, TIMER_FULL_REFRESH);
 KillTimer(hWnd, TIMER_PRIORITY_UPDATE);
 PostQuitMessage(0);
 }
 break;

 default:
 return DefWindowProc(hWnd, message, wParam, lParam);
 }
 return 0;
}

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
 UNREFERENCED_PARAMETER(lParam);
 switch (message) {
 case WM_INITDIALOG:
 return (INT_PTR)TRUE;

 case WM_COMMAND:
 if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
 EndDialog(hDlg, LOWORD(wParam));
 return (INT_PTR)TRUE;
 }
 break;
 }
 return (INT_PTR)FALSE;
}

// Adjust listview column widths proportionally to the client width
static void AdjustListViewColumns(HWND hListView)
{
    if (!hListView) return;
    RECT rc; GetClientRect(hListView, &rc);
    int totalW = rc.right - rc.left;
    if (totalW <= 0) return;

    // Reserve some space for scrollbar and padding
    int scrollW = GetSystemMetrics(SM_CXVSCROLL) + 8;

    // Proportions: Name ~60%, PID ~15%, Priority ~25% (of usable width)
    int usable = totalW - scrollW;
    if (usable < 200) usable = totalW; // fallback

    int pidW = i_max(60, (int)(usable * 0.15));
    int priW = i_max(80, (int)(usable * 0.20));
    int nameW = usable - pidW - priW;
    if (nameW < 100) {
        // clamp and reflow
        nameW = i_max(100, usable - pidW - priW);
    }

    LVCOLUMNW col; ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_WIDTH;
    col.cx = nameW; ListView_SetColumn(hListView, 0, &col);
    col.cx = pidW; ListView_SetColumn(hListView, 1, &col);
    col.cx = priW; ListView_SetColumn(hListView, 2, &col);
}
