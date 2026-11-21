#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <Windows.h>
#include "iup.h"
#include "common.h"
#include "parameters.h"
#include "menu_icons.h"

// ! the order decides which module get processed first
Module* modules[MODULE_CNT] = {
    &lagModule,
    &dropModule,
    &throttleModule,
    &dupModule,
    &oodModule,
    &tamperModule,
    &resetModule,
	&bandwidthModule,
};

volatile short sendState = SEND_STATUS_NONE;

// global iup handlers
static Ihandle *dialog, *topFrame, *bottomFrame; 
static Ihandle *statusLabel;
static Ihandle *filterText, *filterButton, *reapplyButton;
static Ihandle *watchdogIcon;
static BOOL watchdogWarned = FALSE;
static BOOL filteringActive = FALSE;
static BOOL sessionReady = FALSE; /* skip saving on failed launch/elevation */
Ihandle *filterSelectList;
// timer to update icons
static Ihandle *stateIcon;
static Ihandle *timer;
static Ihandle *timeout = NULL;

void showStatus(const char *line);
static int uiOnDialogShow(Ihandle *ih, int state);
static int uiStopCb(Ihandle *ih);
static int uiStartCb(Ihandle *ih);
static int uiReapplyCb(Ihandle *ih);
static int uiLoadParametersCb(Ihandle *ih);
static int uiSaveParametersCb(Ihandle *ih);
static int uiExitCb(Ihandle *ih);
static int uiTimerCb(Ihandle *ih);
static int uiTimeoutCb(Ihandle *ih);
static int uiListSelectCb(Ihandle *ih, char *text, int item, int state);
static int uiFilterTextCb(Ihandle *ih);
static void uiSetupModule(Module *module, Ihandle *parent);

// serializing config files using a stupid custom format
#define CONFIG_FILE "config.txt"
#define CONFIG_MAX_RECORDS 64
#define CONFIG_BUF_SIZE 4096
typedef struct {
    char* filterName;
    char* filterValue;
} filterRecord;
UINT filtersSize;
filterRecord filters[CONFIG_MAX_RECORDS] = {0};
char configBuf[CONFIG_BUF_SIZE+2]; // add some padding to write \n
BOOL parameterized = 0; // parameterized flag, means reading args from command line

// loading up filters and fill in
void loadConfig() {
    char path[MSG_BUFSIZE];
    char *p;
    FILE *f;
    GetModuleFileName(NULL, path, MSG_BUFSIZE);
    LOG("Executable path: %s", path);
    p = strrchr(path, '\\');
    if (p == NULL) p = strrchr(path, '/'); // holy shit
    strcpy(p+1, CONFIG_FILE);
    LOG("Config path: %s", path);
    f = fopen(path, "r");
    if (f) {
        size_t len;
        char *current, *last;
        len = fread(configBuf, sizeof(char), CONFIG_BUF_SIZE, f);
        if (len == CONFIG_BUF_SIZE) {
            LOG("Config file is larger than %d bytes, get truncated.", CONFIG_BUF_SIZE);
        }
        // always patch in a newline at the end to ease parsing
        configBuf[len] = '\n';
        configBuf[len+1] = '\0';

        // parse out the kv pairs. isn't quite safe
        filtersSize = 0;
        last = current = configBuf;
        do {
            // eat up empty lines
EAT_SPACE:  while (isspace(*current)) { ++current; }
            if (*current == '#') {
                current = strchr(current, '\n');
                if (!current) break;
                current = current + 1;
                goto EAT_SPACE;
            }

            // now we can start
            last = current;
            current = strchr(last, ':');
            if (!current) break;
            *current = '\0';
            filters[filtersSize].filterName = last;
            current += 1;
            while (isspace(*current)) { ++current; } // eat potential space after :
            last = current;
            current = strchr(last, '\n');
            if (!current) break;
            filters[filtersSize].filterValue = last;
            *current = '\0';
            if (*(current-1) == '\r') *(current-1) = 0;
            last = current = current + 1;
            ++filtersSize;
        } while (last && last - configBuf < CONFIG_BUF_SIZE);
        LOG("Loaded %u records.", filtersSize);
    }

    if (!f || filtersSize == 0)
    {
        LOG("Failed to load from config. Fill in a simple one.");
        // config is missing or ill-formed. fill in some simple ones
        filters[filtersSize].filterName = "loopback packets";
        filters[filtersSize].filterValue = "outbound and ip.DstAddr >= 127.0.0.1 and ip.DstAddr <= 127.255.255.255";
        filtersSize = 1;
    }
}

void init(int argc, char* argv[]) {
    UINT ix;
    Ihandle *topVbox, *bottomVbox, *dialogVBox, *controlHbox;
    Ihandle *noneIcon, *doingIcon, *errorIcon;
    Ihandle *watchdogGrey, *watchdogGreen, *watchdogRed;
    Ihandle *fileMenu, *menuBar, *loadItem, *saveItem, *exitItem;
    char* arg_value = NULL;

    // fill in config
    loadConfig();

    // iup inits
    IupOpen(&argc, &argv);

    // this is so easy to get wrong so it's pretty worth noting in the program
    statusLabel = IupLabel("NOTICE: When capturing localhost (loopback) packets, you CAN'T include inbound criteria.\n"
        "Filters like 'udp' need to be 'udp and outbound' to work. See readme for more info.");
    IupSetAttribute(statusLabel, "EXPAND", "HORIZONTAL");
    IupSetAttribute(statusLabel, "PADDING", "8x8");

    topFrame = IupFrame(
        topVbox = IupVbox(
            filterText = IupText(NULL),
            controlHbox = IupHbox(
                stateIcon = IupLabel(NULL),
                filterButton = IupButton("Start", NULL),
                reapplyButton = IupButton("Re-apply", NULL),
                watchdogIcon = IupLabel(NULL),
                IupFill(),
                IupLabel("Presets:  "),
                filterSelectList = IupList(NULL),
                NULL
            ),
            NULL
        )
    );

    // parse arguments and set globals *before* setting up UI.
    // arguments can be read and set after callbacks are setup
    // FIXME as Release is built as WindowedApp, stdout/stderr won't show
    LOG("argc: %d", argc);
    if (argc > 1) {
        if (!parseArgs(argc, argv)) {
            fprintf(stderr, "invalid argument count. ensure you're using options as \"--drop on\"");
            exit(-1); // fail fast.
        }
        parameterized = 1;
    }

    IupSetAttribute(topFrame, "TITLE", "Filtering");
    IupSetAttribute(topFrame, "EXPAND", "HORIZONTAL");
    IupSetAttribute(filterText, "EXPAND", "HORIZONTAL");
    IupSetCallback(filterText, "VALUECHANGED_CB", (Icallback)uiFilterTextCb);
    paramsRegister("filter", filterText, PARAM_FILTER);
    IupSetAttribute(filterButton, "PADDING", "8x");
    IupSetCallback(filterButton, "ACTION", uiStartCb);
    IupSetAttribute(reapplyButton, "PADDING", "8x");
    IupSetAttribute(reapplyButton, "ACTIVE", "NO");
    IupSetAttribute(reapplyButton, "TIP", "Stop and restart packet filtering with current settings");
    IupSetCallback(reapplyButton, "ACTION", uiReapplyCb);
    IupSetAttribute(topVbox, "NCMARGIN", "4x4");
    IupSetAttribute(topVbox, "NCGAP", "4x2");
    IupSetAttribute(controlHbox, "ALIGNMENT", "ACENTER");

    // setup state icon
    IupSetAttribute(stateIcon, "IMAGE", "none_icon");
    IupSetAttribute(stateIcon, "PADDING", "4x");
    IupSetAttribute(watchdogIcon, "IMAGE", "watchdog_grey");
    IupSetAttribute(watchdogIcon, "PADDING", "4x");
    IupSetAttribute(watchdogIcon, "TIP", "WinDivert stopped");

    // fill in options and setup callback
    IupSetAttribute(filterSelectList, "VISIBLECOLUMNS", "24");
    IupSetAttribute(filterSelectList, "DROPDOWN", "YES");
    for (ix = 0; ix < filtersSize; ++ix) {
        char ixBuf[4];
        sprintf(ixBuf, "%d", ix+1); // ! staring from 1, following lua indexing
        IupStoreAttribute(filterSelectList, ixBuf, filters[ix].filterName);
    }
    IupSetAttribute(filterSelectList, "VALUE", "1");
    IupSetCallback(filterSelectList, "ACTION", (Icallback)uiListSelectCb);
    paramsRegister("preset", filterSelectList, PARAM_PRESET);
    // set filter text value since the callback won't take effect before main loop starts
    IupSetAttribute(filterText, "VALUE", filters[0].filterValue);

    // functionalities frame 
    bottomFrame = IupFrame(
        bottomVbox = IupVbox(
            NULL
        )
    );
    IupSetAttribute(bottomFrame, "TITLE", "Functions");
    IupSetAttribute(bottomVbox, "NCMARGIN", "4x4");
    IupSetAttribute(bottomVbox, "NCGAP", "4x2");

    // create icons
    noneIcon = IupImage(8, 8, icon8x8);
    doingIcon = IupImage(8, 8, icon8x8);
    errorIcon = IupImage(8, 8, icon8x8);
    IupSetAttribute(noneIcon, "0", "BGCOLOR");
    IupSetAttribute(noneIcon, "1", "224 224 224");
    IupSetAttribute(doingIcon, "0", "BGCOLOR");
    IupSetAttribute(doingIcon, "1", "109 170 44");
    IupSetAttribute(errorIcon, "0", "BGCOLOR");
    IupSetAttribute(errorIcon, "1", "208 70 72");
    IupSetHandle("none_icon", noneIcon);
    IupSetHandle("doing_icon", doingIcon);
    IupSetHandle("error_icon", errorIcon);
    /* Separate small indicator; existing traffic activity icon is unchanged. */
    watchdogGrey = IupImage(8, 8, icon8x8);
    watchdogGreen = IupImage(8, 8, icon8x8);
    watchdogRed = IupImage(8, 8, icon8x8);
    IupSetAttribute(watchdogGrey, "0", "BGCOLOR");
    IupSetAttribute(watchdogGreen, "0", "BGCOLOR");
    IupSetAttribute(watchdogRed, "0", "BGCOLOR");
    IupSetAttribute(watchdogGrey, "1", "150 150 150");
    IupSetAttribute(watchdogGreen, "1", "60 166 80");
    IupSetAttribute(watchdogRed, "1", "210 68 68");
    IupSetHandle("watchdog_grey", watchdogGrey);
    IupSetHandle("watchdog_green", watchdogGreen);
    IupSetHandle("watchdog_red", watchdogRed);

    // setup module uis
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        uiSetupModule(*(modules+ix), bottomVbox);
    }

    // dialog
    dialog = IupDialog(
        dialogVBox = IupVbox(
            topFrame,
            bottomFrame,
            statusLabel,
            NULL
        )
    );

    IupSetAttribute(dialog, "TITLE", "clumsy " CLUMSY_VERSION " (modified version with jitter)");
    IupSetAttribute(dialog, "SIZE", "480x"); // add padding manually to width
    IupSetAttribute(dialog, "RESIZE", "NO");
    IupSetCallback(dialog, "SHOW_CB", (Icallback)uiOnDialogShow);
    IupSetCallback(dialog, "CLOSE_CB", uiExitCb);

    /* File menu is separate from config.txt (filter preset definitions). */
    loadItem = IupItem("Load parameters ...", NULL);
    saveItem = IupItem("Save parameters ...", NULL);
    exitItem = IupItem("Exit", NULL);
    /* IUP menu items need IUP image handles, not Win32 ICON resource names. */
    IupSetAttributeHandle(loadItem, "IMAGE",
        IupImageRGBA(16, 16, clumsy_menu_load_rgba));
    IupSetAttributeHandle(saveItem, "IMAGE",
        IupImageRGBA(16, 16, clumsy_menu_save_rgba));
    IupSetAttributeHandle(exitItem, "IMAGE",
        IupImageRGBA(16, 16, clumsy_menu_exit_rgba));
    IupSetCallback(loadItem, "ACTION", uiLoadParametersCb);
    IupSetCallback(saveItem, "ACTION", uiSaveParametersCb);
    IupSetCallback(exitItem, "ACTION", uiExitCb);
    fileMenu = IupMenu(loadItem, saveItem, IupSeparator(), exitItem, NULL);
    menuBar = IupMenu(IupSubmenu("File", fileMenu), NULL);
    IupSetAttributeHandle(dialog, "MENU", menuBar);


    // global layout settings to affect childrens
    IupSetAttribute(dialogVBox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(dialogVBox, "NCMARGIN", "4x4");
    IupSetAttribute(dialogVBox, "NCGAP", "4x2");

    // setup timer
    timer = IupTimer();
    IupSetAttribute(timer, "TIME", STR(ICON_UPDATE_MS));
    IupSetCallback(timer, "ACTION_CB", uiTimerCb);

    // setup timeout of program
    arg_value = IupGetGlobal("timeout");
    if(arg_value != NULL)
    {
        char valueBuf[16];
        sprintf(valueBuf, "%s000", arg_value);  // convert from seconds to milliseconds

        timeout = IupTimer();
        IupStoreAttribute(timeout, "TIME", valueBuf);
        IupSetCallback(timeout, "ACTION_CB", uiTimeoutCb);
        IupSetAttribute(timeout, "RUN", "YES");
    }

    /* Restore only after all GUI settings are registered; preserve CLI overrides. */
    if (!parameterized) paramsAutoLoad();
}

void startup() {
    // initialize seed
    srand((unsigned int)time(NULL));

    // kickoff event loops
    IupShowXY(dialog, IUP_CENTER, IUP_CENTER);
    IupMainLoop();
    // ! main loop won't return until program exit
}

void cleanup() {
    /* Every graceful exit path, including the program timeout, passes here.
     * Do not overwrite preferences if this instance did not finish startup. */
    if (sessionReady && !parameterized) paramsAutoSave();

    IupDestroy(timer);
    if (timeout) {
        IupDestroy(timeout);
    }

    IupClose();
    endTimePeriod(); // try close if not closing
}

// Parameter files store filter text, every module checkbox and all numeric values.
// Clumsy's existing config.txt is reserved for named filter presets.
static int uiParameterFileCb(int saving)
{
    Ihandle *chooser = IupFileDlg();
    char message[256];
    const char *path;
    int succeeded;
    IupSetAttribute(chooser, "DIALOGTYPE", saving ? "SAVE" : "OPEN");
    IupSetAttribute(chooser, "TITLE", saving ? "Save Clumsy parameters" : "Load Clumsy parameters");
    IupSetAttribute(chooser, "EXTFILTER", "Clumsy parameters (*.ini)|*.ini|All files (*.*)|*.*|");
    IupSetAttribute(chooser, "EXTDEFAULT", "ini");
    if (saving) IupSetAttribute(chooser, "FILE", "clumsy-parameters.ini");
    IupSetAttribute(chooser, "NOCHANGEDIR", "YES");
    IupSetAttributeHandle(chooser, "PARENTDIALOG", dialog);
    IupPopup(chooser, IUP_CENTERPARENT, IUP_CENTERPARENT);
    if (IupGetInt(chooser, "STATUS") < 0) {
        IupDestroy(chooser);
        return IUP_DEFAULT;
    }
    path = IupGetAttribute(chooser, "VALUE");
    succeeded = saving ? paramsSave(path, message, sizeof(message))
                       : paramsLoad(path, message, sizeof(message));
    if (!succeeded) {
        IupMessage(saving ? "Save parameters" : "Load parameters", message);
    } else if (saving) {
        showStatus("Parameters saved.");
    } else {
        showStatus(filteringActive
            ? "Parameters loaded. The new filter takes effect after Re-apply."
            : "Parameters loaded.");
    }
    IupDestroy(chooser);
    return IUP_DEFAULT;
}

static int uiLoadParametersCb(Ihandle *ih)
{
    UNREFERENCED_PARAMETER(ih);
    return uiParameterFileCb(0);
}

static int uiSaveParametersCb(Ihandle *ih)
{
    UNREFERENCED_PARAMETER(ih);
    return uiParameterFileCb(1);
}

static int uiExitCb(Ihandle *ih)
{
    UNREFERENCED_PARAMETER(ih);
    if (filteringActive) uiStopCb(filterButton);
    return IUP_CLOSE;
}

// ui logics
void showStatus(const char *line) {
    IupStoreAttribute(statusLabel, "TITLE", line); 
}

// in fact only 32bit binary would run on 64 bit os
// if this happens pop out message box and exit
static BOOL check32RunningOn64(HWND hWnd) {
    BOOL is64ret;
    // consider IsWow64Process return value
    if (IsWow64Process(GetCurrentProcess(), &is64ret) && is64ret) {
        MessageBox(hWnd, (LPCSTR)"You're running 32bit clumsy on 64bit Windows, which wouldn't work. Please use the 64bit clumsy version.",
            (LPCSTR)"Aborting", MB_OK);
        return TRUE;
    }
    return FALSE;
}

static BOOL checkIsRunning() {
    //It will be closed and destroyed when programm terminates (according to MSDN).
    HANDLE hStartEvent = CreateEventW(NULL, FALSE, FALSE, L"Global\\CLUMSY_IS_RUNNING_EVENT_NAME");

    if (hStartEvent == NULL)
        return TRUE;

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hStartEvent);
        hStartEvent = NULL;
        return TRUE;
    }

    return FALSE;
}


static int uiOnDialogShow(Ihandle *ih, int state) {
    // only need to process on show
    HWND hWnd;
    BOOL exit;
    HICON icon;
    HINSTANCE hInstance;
    if (state != IUP_SHOW) return IUP_DEFAULT;
    hWnd = (HWND)IupGetAttribute(ih, "HWND");
    hInstance = GetModuleHandle(NULL);

    // set application icon
    icon = LoadIcon(hInstance, "CLUMSY_ICON");
    SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);

    exit = checkIsRunning();
    if (exit) {
        MessageBox(hWnd, (LPCSTR)"Theres' already an instance of clumsy running.",
            (LPCSTR)"Aborting", MB_OK);
        return IUP_CLOSE;
    }

#ifdef _WIN32
    exit = check32RunningOn64(hWnd);
    if (exit) {
        return IUP_CLOSE;
    }
#endif

    // try elevate and decides whether to exit
    exit = tryElevate(hWnd, parameterized);

    if (!exit) sessionReady = TRUE;

    if (!exit && parameterized) {
        setFromParameter(filterText, "VALUE", "filter");
        LOG("is parameterized, start filtering upon execution.");
        uiStartCb(filterButton);
    }

    return exit ? IUP_CLOSE : IUP_DEFAULT;
}

static int uiStartCb(Ihandle *ih) {
    char buf[MSG_BUFSIZE];
    UNREFERENCED_PARAMETER(ih);
    if (divertStart(IupGetAttribute(filterText, "VALUE"), buf) == 0) {
        filteringActive = FALSE;
        IupSetAttribute(reapplyButton, "ACTIVE", "NO");
        IupSetAttribute(watchdogIcon, "IMAGE", "watchdog_grey");
        IupSetAttribute(watchdogIcon, "TIP", "WinDivert stopped");
        showStatus(buf);
        return IUP_DEFAULT;
    }

    // successfully started: the filter remains editable, and Re-apply commits edits.
    filteringActive = TRUE;
    watchdogWarned = FALSE;
    IupSetAttribute(watchdogIcon, "IMAGE", "watchdog_green");
    IupSetAttribute(watchdogIcon, "TIP", "WinDivert watchdog healthy");
    showStatus("Started filtering. Changes to the filter take effect after Re-apply.");
    IupSetAttribute(filterButton, "TITLE", "Stop");
    IupSetAttribute(reapplyButton, "ACTIVE", "YES");
    IupSetCallback(filterButton, "ACTION", uiStopCb);
    IupSetAttribute(timer, "RUN", "YES");

    return IUP_DEFAULT;
}

static int uiStopCb(Ihandle *ih) {
    int ix;
    UNREFERENCED_PARAMETER(ih);
    if (!filteringActive) return IUP_DEFAULT;

    // Disable Re-apply before shutting down; never leave it active while stopped.
    IupSetAttribute(reapplyButton, "ACTIVE", "NO");
    IupSetAttribute(filterButton, "ACTIVE", "NO");
    IupFlush(); // flush to show disabled state
    divertStop();
    filteringActive = FALSE;
    watchdogWarned = FALSE;
    IupSetAttribute(watchdogIcon, "IMAGE", "watchdog_grey");
    IupSetAttribute(watchdogIcon, "TIP", "WinDivert stopped");

    IupSetAttribute(filterText, "ACTIVE", "YES");
    IupSetAttribute(filterButton, "TITLE", "Start");
    IupSetAttribute(filterButton, "ACTIVE", "YES");
    IupSetCallback(filterButton, "ACTION", uiStartCb);

    // stop timer and clean up icons
    IupSetAttribute(timer, "RUN", "NO");
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        modules[ix]->processTriggered = 0; // use = here since is threads already stopped
        IupSetAttribute(modules[ix]->iconHandle, "IMAGE", "none_icon");
    }
    sendState = SEND_STATUS_NONE;
    IupSetAttribute(stateIcon, "IMAGE", "none_icon");

    showStatus("Stopped. To begin again, edit criteria and click Start.");
    return IUP_DEFAULT;
}

// Re-apply uses the same stop/start lifecycle as the original button.
// It does not stop the Windows WinDivert driver service or affect other users.
static int uiReapplyCb(Ihandle *ih) {
    UNREFERENCED_PARAMETER(ih);
    if (!filteringActive) return IUP_DEFAULT;

    IupSetAttribute(reapplyButton, "ACTIVE", "NO");
    uiStopCb(filterButton);
    uiStartCb(filterButton);

    if (filteringActive) {
        showStatus("Re-applied: packet filtering restarted with current settings.");
    }
    // If starting failed, uiStartCb leaves Re-apply disabled and shows the error.
    return IUP_DEFAULT;
}

static int uiToggleControls(Ihandle *ih, int state) {
    Ihandle *controls = (Ihandle*)IupGetAttribute(ih, CONTROLS_HANDLE);
    short *target = (short*)IupGetAttribute(ih, SYNCED_VALUE);
    int controlsActive = IupGetInt(controls, "ACTIVE");
    if (controlsActive && !state) {
        IupSetAttribute(controls, "ACTIVE", "NO");
        InterlockedExchange16(target, I2S(state));
    } else if (!controlsActive && state) {
        IupSetAttribute(controls, "ACTIVE", "YES");
        InterlockedExchange16(target, I2S(state));
    }

    return IUP_DEFAULT;
}

static int uiTimerCb(Ihandle *ih) {
    int ix;
    UNREFERENCED_PARAMETER(ih);
    if (filteringActive) {
        int health = divertWatchdogHealth();
        if (health == 2) {
            IupSetAttribute(watchdogIcon, "IMAGE", "watchdog_red");
            IupSetAttribute(watchdogIcon, "TIP", "WinDivert watchdog failed; re-apply to recover");
            if (!watchdogWarned) {
                watchdogWarned = TRUE;
                showStatus("WARNING: WinDivert heartbeat or packet worker stalled. "
                           "Interception will fail open. Click Re-apply to recover.");
            }
        } else if (health == 1) {
            IupSetAttribute(watchdogIcon, "IMAGE", "watchdog_green");
            IupSetAttribute(watchdogIcon, "TIP", "WinDivert watchdog healthy");
        }
    }
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        if (modules[ix]->processTriggered) {
            IupSetAttribute(modules[ix]->iconHandle, "IMAGE", "doing_icon");
            InterlockedAnd16(&(modules[ix]->processTriggered), 0);
        } else {
            IupSetAttribute(modules[ix]->iconHandle, "IMAGE", "none_icon");
        }
    }

    // update global send status icon
    switch (sendState)
    {
    case SEND_STATUS_NONE:
        IupSetAttribute(stateIcon, "IMAGE", "none_icon");
        break;
    case SEND_STATUS_SEND:
        IupSetAttribute(stateIcon, "IMAGE", "doing_icon");
        InterlockedAnd16(&sendState, SEND_STATUS_NONE);
        break;
    case SEND_STATUS_FAIL:
        IupSetAttribute(stateIcon, "IMAGE", "error_icon");
        InterlockedAnd16(&sendState, SEND_STATUS_NONE);
        break;
    }

    return IUP_DEFAULT;
}

static int uiTimeoutCb(Ihandle *ih) {
    UNREFERENCED_PARAMETER(ih);
    return IUP_CLOSE;
 }

static int uiListSelectCb(Ihandle *ih, char *text, int item, int state) {
    UNREFERENCED_PARAMETER(text);
    UNREFERENCED_PARAMETER(ih);
    if (state == 1) {
        IupSetAttribute(filterText, "VALUE", filters[item-1].filterValue);
    }
    return IUP_DEFAULT;
}

static int uiFilterTextCb(Ihandle *ih)  {
    UNREFERENCED_PARAMETER(ih);
    // unselect list
    IupSetAttribute(filterSelectList, "VALUE", "0");
    return IUP_DEFAULT;
}

static void uiSetupModule(Module *module, Ihandle *parent) {
    Ihandle *groupBox, *toggle, *controls, *icon;
    char settingKey[64];
    groupBox = IupHbox(
        icon = IupLabel(NULL),
        toggle = IupToggle(module->displayName, NULL),
        IupFill(),
        controls = module->setupUIFunc(),
        NULL
    );
    IupSetAttribute(groupBox, "EXPAND", "HORIZONTAL");
    IupSetAttribute(groupBox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(controls, "ALIGNMENT", "ACENTER");
    IupAppend(parent, groupBox);

    // set controls as attribute to toggle and enable toggle callback
    IupSetCallback(toggle, "ACTION", (Icallback)uiToggleControls);
    IupSetAttribute(toggle, CONTROLS_HANDLE, (char*)controls);
    IupSetAttribute(toggle, SYNCED_VALUE, (char*)module->enabledFlag);
    snprintf(settingKey, sizeof(settingKey), "%s.enabled", module->shortName);
    paramsRegister(settingKey, toggle, PARAM_MODULE);
    IupSetAttribute(controls, "ACTIVE", "NO"); // startup as inactive
    IupSetAttribute(controls, "NCGAP", "4"); // startup as inactive

    // set default icon
    IupSetAttribute(icon, "IMAGE", "none_icon");
    IupSetAttribute(icon, "PADDING", "4x");
    module->iconHandle = icon;

    // parameterize toggle
    if (parameterized) {
        setFromParameter(toggle, "VALUE", module->shortName);
    }
}

int main(int argc, char* argv[]) {
    LOG("Is Run As Admin: %d", IsRunAsAdmin());
    LOG("Is Elevated: %d", IsElevated());
    init(argc, argv);
    startup();
    cleanup();
    return 0;
}
