/*
 * This file is part of the Code::Blocks IDE and licensed under the GNU General Public License, version 3
 * http://www.gnu.org/licenses/gpl-3.0.html
 *
 * $Revision: 13300 $
 * $Id: app.cpp 13300 2023-05-30 16:05:19Z mortenmacfly $
 * $HeadURL: https://svn.code.sf.net/p/codeblocks/code/trunk/src/src/app.cpp $
 */

#include <sdk.h>
#include "app.h"

#include <wx/arrstr.h>
#include <wx/choicdlg.h>
#include <wx/clipbrd.h>
#include <wx/cmdline.h>
#include <wx/debugrpt.h>
#include <wx/filefn.h>
#include <wx/fs_zip.h>
#include <wx/fs_mem.h>
#include <wx/ipc.h>
#include <wx/log.h> // for wxSafeShowMessage()
#include <wx/msgdlg.h>
#include <wx/msgout.h>
#include <wx/notebook.h>
#include <wx/regex.h>
#include <wx/stdpaths.h>
#include <wx/xrc/xmlres.h>

#include "appglobals.h"
#include "associations.h"
#include "cbauibook.h"
#include "cbexception.h"
#include "cbstyledtextctrl.h"
#include "configmanager.h"
#include "compiler.h"
#include "compilerfactory.h"
#include "crashhandler.h"
#include "debuggermanager.h"
#include "editormanager.h"
#include "globals.h"
#include "loggers.h"
#include "logmanager.h"
#include "macrosmanager.h"
#include "manager.h"
#include "personalitymanager.h"
#include "pluginmanager.h"
#include "projectmanager.h"
#include "projectmanagerui.h"
#include "scriptingmanager.h"
#include "sdk_events.h"
#include "splashscreen.h"
#include "uservarmanager.h"
#include "uservardlgs.h"

#if defined(__APPLE__) && defined(__MACH__)
    #include <sys/param.h>
    #include <mach-o/dyld.h>
#endif

#ifndef CB_PRECOMP
    #include <wx/dir.h>
    #include "xtra_res.h"
    #include "filemanager.h" // LoaderBase
    #include "cbproject.h"
#endif

#ifndef APP_PREFIX
    #define APP_PREFIX ""
#endif

#ifdef __WXMSW__
    #include "exchndl.h"         // Crash handler DLL -> includes windows.h, therefore
    #include <wx/msw/winundef.h> // ...include this header file on the NEXT LINE (wxWidgets docs say so)
    #include <commctrl.h>
#endif

#ifndef __WXMAC__
inline wxString GetResourcesDir(){ return wxEmptyString; }
#endif

namespace
{
bool s_Loading = false;

class DDEServer : public wxServer
{
    public:
        DDEServer(MainFrame* frame) : m_Frame(frame) { ; }
        wxConnectionBase *OnAcceptConnection(const wxString& topic) override;
        MainFrame* GetFrame()                 { return m_Frame;  }
        void       SetFrame(MainFrame* frame) { m_Frame = frame; }
    private:
        MainFrame* m_Frame;
};

class DDEConnection : public wxConnection
{
    public:
        DDEConnection(MainFrame* frame) : m_Frame(frame) { ; }
        bool OnExecute(const wxString& topic, const void *data, size_t size,
                       wxIPCFormat format) override;
        bool OnDisconnect() override;
    private:
        MainFrame* m_Frame;
};

wxConnectionBase* DDEServer::OnAcceptConnection(const wxString& topic)
{
    return topic == DDE_TOPIC ? new DDEConnection(m_Frame) : nullptr;
}

bool DDEConnection::OnExecute(cb_unused const wxString& topic, const void *data, size_t size,
                              wxIPCFormat format)
{
    const wxString strData = wxConnection::GetTextFromData(data, size, format);

    if (strData.StartsWith(_T("[IfExec_Open(\"")))
        return false; // let Shell Open handle the request as we *know* that we have registered the Shell Open command, too

    if (strData.StartsWith(_T("[Open(\"")))
    {
        wxRegEx reCmd(_T("\"(.*)\""));
        if (reCmd.Matches(strData))
        {
            const wxString file = reCmd.GetMatch(strData, 1);
            // always put files in the delayed queue, the will either be loaded in OnDisconnect, or after creating of MainFrame
            // if we open the files directly it can lead to an applicaton hang (at least when opening C::B's project-file on linux)
            CodeBlocksApp* cb = (CodeBlocksApp*)wxTheApp;
            if (cb)
                cb->AddFileToOpenDelayed(file);
        }
        return true;
    }
    else if (strData.StartsWith(_T("[OpenLine(\"")))
    {
        wxRegEx reCmd(_T("\"(.*)\""));
        if (reCmd.Matches(strData))
        {
            wxString file = reCmd.GetMatch(strData, 1);
            CodeBlocksApp* cb = (CodeBlocksApp*)wxTheApp;
            cb->SetAutoFile(file);
        }
        return true;
    }
    else if (strData.StartsWith(_T("[Raise]")))
    {
        if (m_Frame)
        {
            if (m_Frame->IsIconized())
                m_Frame->Iconize(false);
            m_Frame->Raise();
        }
        return true;
    }
    else if (strData.StartsWith(_T("[CmdLine({")))
    {
        wxString cmdLine, cwd;

        const wxString::size_type posCwd = strData.find("})CWD({");
        if (posCwd != wxString::npos)
        {
            const int coundCmdLind = cbCountOf("[CmdLine({") - 1;
            const int countCWD = cbCountOf("})CWD({") - 1;

            cmdLine = strData.substr(coundCmdLind, posCwd - coundCmdLind);
            cmdLine.Replace("\\)", ")");
            cmdLine.Replace("\\(", "(");

            const wxString::size_type posEnd = strData.find("})]", posCwd + countCWD);
            if (posEnd != wxString::npos)
            {
                cwd = strData.substr(posCwd + countCWD, posEnd - (posCwd + countCWD));
                cwd.Replace("\\)", ")");
                cwd.Replace("\\(", "(");
            }
        }

        if (!cmdLine.empty() && !cwd.empty() && m_Frame)
        {
            CodeBlocksApp* cb = (CodeBlocksApp*)wxTheApp;
            cb->ParseCmdLine(m_Frame, cmdLine, cwd);
            CodeBlocksEvent event(cbEVT_APP_CMDLINE);
            event.SetString(cmdLine);
            event.SetBuildTargetName(cwd);
            Manager::Get()->ProcessEvent(event);
        }
        return true;
    }
    wxSafeShowMessage(wxT("Warning"),wxString::Format(wxT("DDE topic %s not handled."),strData.wx_str()));
    return false;
}

bool DDEConnection::OnDisconnect()
{
    // delayed files will be loaded automatically if MainFrame already exists,
    // otherwise it happens automatically in OnInit after MainFrame is created
    if (!s_Loading && m_Frame)
    {
        CodeBlocksApp* cb = (CodeBlocksApp*)wxTheApp;
        cb->LoadDelayedFiles(m_Frame);
        cb->AttachDebugger();
    }
    return true;
}

DDEServer* g_DDEServer = nullptr;

class DDEClient: public wxClient {
    public:
        DDEClient(void) {}
        wxConnectionBase *OnMakeConnection(void) override { return new DDEConnection(nullptr); }
};

#if wxUSE_CMDLINE_PARSER
#define CMD_ENTRY(X) X
const wxCmdLineEntryDesc cmdLineDesc[] =
{
    { wxCMD_LINE_SWITCH, CMD_ENTRY("h"),  CMD_ENTRY("help"),                  CMD_ENTRY("show this help message"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_OPTION_HELP },
    { wxCMD_LINE_SWITCH, CMD_ENTRY("?"),  CMD_ENTRY("?"),                     CMD_ENTRY("show this help message (alias for help)"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_OPTION_HELP },
    { wxCMD_LINE_SWITCH, CMD_ENTRY(""),   CMD_ENTRY("safe-mode"),             CMD_ENTRY("load in safe mode (all plugins will be disabled)"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
#ifdef __WXMSW__
    { wxCMD_LINE_SWITCH, CMD_ENTRY("na"), CMD_ENTRY("no-check-associations"), CMD_ENTRY("don't perform any association checks"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, CMD_ENTRY("nd"), CMD_ENTRY("no-dde"),                CMD_ENTRY("don't start a DDE server"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
#endif
#ifndef __WXMSW__
    { wxCMD_LINE_SWITCH, CMD_ENTRY("ni"), CMD_ENTRY("no-ipc"),                CMD_ENTRY("don't start an IPC server"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
#endif
    { wxCMD_LINE_SWITCH, CMD_ENTRY("ns"), CMD_ENTRY("no-splash-screen"),      CMD_ENTRY("don't display a splash screen while loading"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, CMD_ENTRY(""),   CMD_ENTRY("multiple-instance"),     CMD_ENTRY("allow running multiple instances"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, CMD_ENTRY("d"),  CMD_ENTRY("debug-log"),             CMD_ENTRY("display application's debug log"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, CMD_ENTRY("nc"), CMD_ENTRY("no-crash-handler"),      CMD_ENTRY("don't use the crash handler (useful for debugging C::B)"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, CMD_ENTRY("v"),  CMD_ENTRY("verbose"),               CMD_ENTRY("show more debugging messages"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_OPTION, CMD_ENTRY(""),   CMD_ENTRY("prefix"),                CMD_ENTRY("the shared data dir prefix"),
      wxCMD_LINE_VAL_STRING, wxCMD_LINE_NEEDS_SEPARATOR },
    { wxCMD_LINE_OPTION, CMD_ENTRY(""),   CMD_ENTRY("user-data-dir"),         CMD_ENTRY("set a custom location for user settings and plugins"),
      wxCMD_LINE_VAL_STRING, wxCMD_LINE_NEEDS_SEPARATOR },
    { wxCMD_LINE_OPTION, CMD_ENTRY("p"),  CMD_ENTRY("personality"),           CMD_ENTRY("the personality to use: \"ask\" or <personality-name>"),
      wxCMD_LINE_VAL_STRING, wxCMD_LINE_NEEDS_SEPARATOR },
    { wxCMD_LINE_SWITCH, CMD_ENTRY(""),   CMD_ENTRY("no-log"),                CMD_ENTRY("turn off the application log"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, CMD_ENTRY(""),   CMD_ENTRY("log-to-file"),           CMD_ENTRY("redirect application log to a file"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, CMD_ENTRY(""),   CMD_ENTRY("debug-log-to-file"),     CMD_ENTRY("redirect application debug log to a file"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_OPTION, CMD_ENTRY(""),   CMD_ENTRY("profile"),               CMD_ENTRY("synonym to personality"),
      wxCMD_LINE_VAL_STRING, wxCMD_LINE_NEEDS_SEPARATOR },
    // Command line for global user variables
    { wxCMD_LINE_SWITCH, CMD_ENTRY("S"),  CMD_ENTRY("set"),                   CMD_ENTRY("specify the active global user variable set"),
      wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, CMD_ENTRY("D"),  CMD_ENTRY(""),                      CMD_ENTRY("set value for global variable. For example: -D [set.]name[.member]=value to set the optional \"member\" value of variable \"name\" in the optional \"set\" to \"value\""),
      wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
    // Build command lines
    { wxCMD_LINE_SWITCH, CMD_ENTRY(""),   CMD_ENTRY("rebuild"),               CMD_ENTRY("clean and then build the project/workspace"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, CMD_ENTRY(""),   CMD_ENTRY("build"),                 CMD_ENTRY("just build the project/workspace"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, CMD_ENTRY(""),   CMD_ENTRY("clean"),                 CMD_ENTRY("clean the project/workspace"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_OPTION, CMD_ENTRY(""),   CMD_ENTRY("target"),                CMD_ENTRY("the target for the batch build"),
      wxCMD_LINE_VAL_STRING, wxCMD_LINE_NEEDS_SEPARATOR },
    { wxCMD_LINE_SWITCH, CMD_ENTRY(""),   CMD_ENTRY("no-batch-window-close"), CMD_ENTRY("do not auto-close log window when batch build is done"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_SWITCH, CMD_ENTRY(""),   CMD_ENTRY("batch-build-notify"),    CMD_ENTRY("show message when batch build is done"),
      wxCMD_LINE_VAL_NONE, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_OPTION, CMD_ENTRY(""),   CMD_ENTRY("script"),                CMD_ENTRY("execute script file"),
      wxCMD_LINE_VAL_STRING, wxCMD_LINE_NEEDS_SEPARATOR },
    { wxCMD_LINE_OPTION, CMD_ENTRY(""),   CMD_ENTRY("file"),                  CMD_ENTRY("open file and optionally jump to specific line (file[:line])"),
      wxCMD_LINE_VAL_STRING, wxCMD_LINE_NEEDS_SEPARATOR },
    { wxCMD_LINE_OPTION, CMD_ENTRY(""),   CMD_ENTRY("dbg-config"),            CMD_ENTRY("selects the debugger config used for attaching (uses the current selected target if omitted) "),
      wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL | wxCMD_LINE_NEEDS_SEPARATOR },
    { wxCMD_LINE_OPTION, CMD_ENTRY(""),   CMD_ENTRY("dbg-attach"),            CMD_ENTRY("string passed to the debugger plugin which is used for attaching to a process"),
      wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL | wxCMD_LINE_NEEDS_SEPARATOR },
    { wxCMD_LINE_PARAM,  CMD_ENTRY(""),   CMD_ENTRY(""),                      CMD_ENTRY("filename(s)"),
      wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL | wxCMD_LINE_PARAM_MULTIPLE },
    { wxCMD_LINE_NONE }
};
#endif // wxUSE_CMDLINE_PARSER

class Splash
{
    public:
        Splash(const bool show) : m_pSplash(nullptr)
        {
            if (show)
            {
                wxBitmap bmp = cbLoadBitmap(ConfigManager::ReadDataPath() + _T("/images/splash_1312.png"));
                wxMemoryDC dc;
                dc.SelectObject(bmp);
                cbSplashScreen::DrawReleaseInfo(dc);
                dc.SelectObject(wxNullBitmap);
                m_pSplash = new cbSplashScreen(bmp);
                Manager::Yield();
            }
        }
        ~Splash()
        {
            Hide();
        }
        void Hide()
        {
            if (m_pSplash)
            {
                m_pSplash->Destroy();
                m_pSplash = nullptr;
            }
        }

    private:
        cbSplashScreen* m_pSplash;
};

class cbMessageOutputNull : public wxMessageOutput
{
public:

    virtual void Output(const wxString &str) override;
};

void cbMessageOutputNull::Output(cb_unused const wxString &str){}
} // namespace

IMPLEMENT_APP(CodeBlocksApp) // TODO: This gives a "redundant declaration" warning, though I think it's false. Dig through macro and check.

BEGIN_EVENT_TABLE(CodeBlocksApp, wxApp)
    EVT_ACTIVATE_APP(CodeBlocksApp::OnAppActivate)
    EVT_TASKBAR_LEFT_DOWN(CodeBlocksApp::OnTBIconLeftDown)
END_EVENT_TABLE()

#ifdef __WXMAC__
#include "wx/osx/core/cfstring.h"
#include "wx/intl.h"

#include <CoreFoundation/CFBundle.h>
#include <CoreFoundation/CFURL.h>

// returns e.g. "/Applications/appname.app/Contents/Resources" if application is bundled,
// or the directory of the binary, e.g. "/usr/local/bin/appname", if it is *not* bundled.
static wxString GetResourcesDir()
{
    CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(CFBundleGetMainBundle());
    CFURLRef absoluteURL = CFURLCopyAbsoluteURL(resourcesURL); // relative -> absolute
    CFRelease(resourcesURL);
    CFStringRef cfStrPath = CFURLCopyFileSystemPath(absoluteURL,kCFURLPOSIXPathStyle);
    CFRelease(absoluteURL);
      return wxCFStringRef(cfStrPath).AsString(wxLocale::GetSystemEncoding());
}
#endif

bool CodeBlocksApp::LoadConfig()
{
    if (m_UserDataDir!=wxEmptyString)
    {
        // if --user-data-dir=path was specified we tell
        //ConfigManager (and CfgMgrBldr) about it, which will propagate
        //it through the app and plugins
        if ( !ConfigManager::SetUserDataFolder(m_UserDataDir) )
            return false;
    }

    ConfigManager *cfg = Manager::Get()->GetConfigManager(_T("app"));

    wxString data(wxT(APP_PREFIX));

    if (platform::windows)
        data.assign(GetAppPath());
    else if (platform::macosx)
    {
        data.assign(GetResourcesDir());                 // CodeBlocks.app/Contents/Resources
        if (!data.Contains(wxString(_T("/Resources")))) // not a bundle, use relative path
            data = GetAppPath() + _T("/..");
    }

    if (data.IsEmpty())
    {
        data.assign(GetAppPath());  // fallback
        data.Replace(_T("/bin"),_T(""));
    }


    if (!m_Prefix.IsEmpty())        // --prefix command line switch overrides builtin value
        data = m_Prefix;
    else                            // also, check for environment
    {

        wxString env;
        wxGetEnv(_T("CODEBLOCKS_DATA_DIR"), &env);
        if (!env.IsEmpty())
            data = env;
    }

    data.append(_T("/share/codeblocks"));

    // Make sure the path to our resources is always an absolute path, because resource loading
    // would fail with a relative path if some part of the code changes the current working
    // directory.
    wxFileName filename(data);
    if (filename.IsRelative())
        filename.MakeAbsolute();
    data = filename.GetFullPath();

    cfg->Write(_T("data_path"), data);

    //m_HasDebugLog = Manager::Get()->GetConfigManager(_T("message_manager"))->ReadBool(_T("/has_debug_log"), false) || m_HasDebugLog;
    //Manager::Get()->GetConfigManager(_T("message_manager"))->Write(_T("/has_debug_log"), m_HasDebugLog);

    return true;
}

void CodeBlocksApp::InitAssociations()
{
#ifdef __WXMSW__
    ConfigManager *cfg = Manager::Get()->GetConfigManager(_T("app"));
    if (m_Assocs && cfg->ReadBool(_T("/environment/check_associations"), true))
    {
        if (!Associations::Check())
        {
            AskAssocDialog dlg(Manager::Get()->GetAppWindow());
            PlaceWindow(&dlg);

            switch(dlg.ShowModal())
            {
            case ASC_ASSOC_DLG_NO_DONT_ASK:
                cfg->Write(_T("/environment/check_associations"), false);
                break;
            case ASC_ASSOC_DLG_NO_ONLY_NOW:
                break;
            case ASC_ASSOC_DLG_YES_C_FILES:
                Associations::SetCore();
                break;
            case ASC_ASSOC_DLG_YES_ALL_FILES:
                Associations::SetAll();
                break;
            default:
                break;
            };
        }
    }
#endif
}

void CodeBlocksApp::InitDebugConsole()
{
#ifdef __WXMSW__
    #ifdef __CBDEBUG__
    // Remember to compile as a console application!
    AllocConsole();
    HANDLE myhandle = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD co = {80,2000};
    SetConsoleScreenBufferSize(myhandle, co);
    fprintf(stdout,"CONSOLE DEBUG ACTIVATED\n");
    // wxLogWindow *myerr = new wxLogWindow(nullptr,"debug");
    #endif
#endif
}

void CodeBlocksApp::InitExceptionHandler()
{
#ifdef __WXMSW__
    ExcHndlInit();

    // If the executable folder is not writable, as it happens with releases installed under Program Files,
    // move crash report to the configuration folder and give it a more useful name
    if (!wxFileName::IsDirWritable(ConfigManager::GetFolder(sdBase)))
    {
        wxString release(RELEASE);

#if SVN_BUILD
        release << ConfigManager::GetRevisionString();
#endif

        const wxString dtDisplay(wxDateTime::Now().Format("%Y%m%d_%H%M%S"));  // year first, so alphabetical orden matches cronological order
        m_crashReportName = ConfigManager::GetFolder(sdConfig)+
                            wxFileName::GetPathSeparator()+
                            wxString::Format("CodeBlocks_%s_%s.rpt", dtDisplay, release);

        ExcHndlSetLogFileNameA(m_crashReportName.c_str());
    }
#endif
}

bool CodeBlocksApp::InitXRCStuff()
{
    if ( !Manager::LoadResource(_T("resources.zip")) )
    {

        wxString msg;
        msg.Printf(_T("Cannot find resources...\n"
                      "%s was configured to be installed in '%s'.\n"
                      "Please use the command-line switch '--prefix' or "
                      "set the CODEBLOCKS_DATA_DIR environment variable "
                      "to point where %s is installed,\n"
                      "or try re-installing the application..."),
                   appglobals::AppName.wx_str(),
                   ConfigManager::ReadDataPath().wx_str(),
                   appglobals::AppName.wx_str());
        cbMessageBox(msg);

        return false;
    }
    return true;
}

MainFrame* CodeBlocksApp::InitFrame()
{
    static_assert(wxMinimumVersion<3,0,0>::eval, "wxWidgets 3.0.0 or higher is required");

    MainFrame *frame = new MainFrame();
    SetTopWindow(nullptr);

    if (g_DDEServer && m_DDE)
        g_DDEServer->SetFrame(frame); // Set m_Frame in DDE-Server

    return frame;
}

void CodeBlocksApp::CheckVersion()
{
    // This is a remnant from early 2006 (Windows only), but keep the revision tag for possible future use
    ConfigManager *cfg = Manager::Get()->GetConfigManager(_T("app"));

    if (cfg->Read(_T("version")) != appglobals::AppActualVersion)
        cfg->Write(_T("version"), appglobals::AppActualVersion);
}

void CodeBlocksApp::InitLocale()
{
    ConfigManager* cfg = Manager::Get()->GetConfigManager(_T("app"));

    wxString path(ConfigManager::GetDataFolder() + _T("/locale"));

    if (cfg->ReadBool(_T("/locale/enable"), false) == false)
        return;

    wxString lang(cfg->Read(_T("/locale/language")));

    wxLocale::AddCatalogLookupPathPrefix(path);


    const wxLanguageInfo *info;

    if (!lang.IsEmpty()) // Note: You can also write this line of code as !(!lang) from wx-2.9 onwards
        info = wxLocale::FindLanguageInfo(lang);
    else
        info = wxLocale::GetLanguageInfo(wxLANGUAGE_DEFAULT);

    if (info == nullptr) // should never happen, but who knows...
        return;

    m_locale.Init(info->Language);

    path.Alloc(path.length() + 10);
    path.Append(_T('/'));
    path.Append(info->CanonicalName);

    if ( !wxDirExists(path) )
        return;

    wxDir dir(path);
    if (!dir.IsOpened())
        return;

    wxString moName;

    if (dir.GetFirst(&moName, _T("*.mo"), wxDIR_FILES))
    {
        do
        {
            // Extension is added unconditionally in AddCatalog() since wxWidgets
            // commit b9a9ae7 (just before release of wx3.1.6), so file.mo is converted in file.mo.mo
            // Removing the extension is backwards compatible (it was not supposed to be there)
            m_locale.AddCatalog(moName.BeforeLast('.'));
        } while (dir.GetNext(&moName));
    }
}

bool CodeBlocksApp::OnInit()
{
#ifdef __WXMSW__
    MSWEnableDarkMode(DarkMode_Always); // Force dark mode
    InitCommonControls();
#endif

    wxLog::EnableLogging(true);

    SetAppName("codeblocks");

    s_Loading              = true;
    m_pBatchBuildDialog    = nullptr;
    m_BatchExitCode        = 0;
    m_Batch                = false;
    m_BatchNotify          = false;
    m_Build                = false;
    m_ReBuild              = false;
    m_Clean                = false;
    m_HasProject           = false;
    m_HasWorkSpace         = false;
    m_SafeMode             = false;
    m_BatchWindowAutoClose = true;
    m_pSingleInstance      = nullptr;

    if (wxTheClipboard->IsOpened())
        wxTheClipboard->Flush();

    wxCmdLineParser& parser = *Manager::GetCmdLineParser();
    parser.SetDesc(cmdLineDesc);

    // NOTE: crash handler explicitly disabled because it causes problems
    //       with plugins loading/unloading...
    //
    // static CrashHandler crash_handler(!m_CrashHandler);

    // we'll do this once and for all at startup
    wxFileSystem::AddHandler(new wxZipFSHandler);
    wxFileSystem::AddHandler(new wxMemoryFSHandler);
    wxToolBarAddOnXmlHandler *toolbarAddonHandler = new wxToolBarAddOnXmlHandler;
    wxXmlResource::Get()->InsertHandler(toolbarAddonHandler);
    wxXmlResource::Get()->InsertHandler(new wxScrollingDialogXmlHandler);
    wxInitAllImageHandlers();
    wxXmlResource::Get()->InitAllHandlers();

    Manager::SetToolbarHandler(toolbarAddonHandler);

    LogManager *log = Manager::Get()->GetLogManager();
    // Translating this string is futile, the locale has not been initialized yet
    log->Log(wxString::Format("Starting %s %s %s", appglobals::AppName,
                              appglobals::AppActualVersionVerb, appglobals::AppBuildTimestamp));

    try
    {
    #if (wxUSE_ON_FATAL_EXCEPTION == 1)
        wxHandleFatalExceptions(true);
    #endif

        InitExceptionHandler();

        delete wxMessageOutput::Set(new cbMessageOutputNull); // No output. (suppress warnings about unknown options from plugins)
        if (ParseCmdLine(nullptr) == -1) // only abort if '--help' was passed in the command line
        {
            delete wxMessageOutput::Set(new wxMessageOutputMessageBox);
            parser.Usage();
            return false;
        }

        // The personality should be set by the ParseCmdLine.
        // If not the "default" would be used. If not called here LoadConfig would fail.
        Manager::Get()->GetPersonalityManager()->MarkAsReady();

        if (!LoadConfig())
            return false;

        // set safe-mode appropriately
        PluginManager::SetSafeMode(m_SafeMode);

        // If not in batch mode, and no startup-script defined, initialise XRC
        if (!m_Batch && m_Script.IsEmpty() && !InitXRCStuff())
            return false;

        InitLocale();

        ConfigManager *appCfg = Manager::Get()->GetConfigManager("app");
        if (m_DDE && !m_Batch && appCfg->ReadBool("/environment/use_ipc", true))
        {
            // Create a new client
            DDEClient* client = new DDEClient;
            DDEConnection* connection = nullptr;
            wxLogNull ln; // own error checking implemented -> avoid debug warnings
            connection = (DDEConnection *)client->MakeConnection("localhost",
                                                                 wxString::Format(DDE_SERVICE,
                                                                                  wxGetUserId()),
                                                                 DDE_TOPIC);
            if (connection)
            {
                // don't eval here just forward the whole command line to the other instance
                wxString cmdLine;
                for (int i = 1 ; i < argc; ++i)
                    cmdLine += wxString(argv[i]) + ' ';

                if (!cmdLine.IsEmpty())
                {
                    // escape openings and closings so it is easily possible to find the end on the rx side
                    cmdLine.Replace("(", "\\(");
                    cmdLine.Replace(")", "\\)");
                    connection->Execute("[CmdLine({" + cmdLine + "})CWD({" + wxGetCwd() + "})]");
                }

                // On Linux, C::B has to be raised explicitly if it's wanted
                if (appCfg->ReadBool("/environment/raise_via_ipc", true))
                    connection->Execute("[Raise]");

                connection->Disconnect();
                delete connection;
                delete client;

                log->Log("Ending application because another instance has been detected!");

                // return false to end the application
                return false;
            }

            // free memory DDE-/IPC-clients, if we are here connection could not be established and there is no need to free it
            delete client;
        }

        if (appCfg->ReadBool("/environment/single_instance", true)
            && !parser.Found("multiple-instance"))
        {
            const wxString name = wxString::Format("Code::Blocks-%s", wxGetUserId());

            m_pSingleInstance = new wxSingleInstanceChecker(name, ConfigManager::GetTempFolder());
            if (m_pSingleInstance->IsAnotherRunning())
            {
                /* NOTE: Due to a recent change in logging code, this visual warning got disabled.
                   So the wxLogError() has been changed to a cbMessageBox(). */
                cbMessageBox(_("Another program instance is already running.\nCode::Blocks is currently configured to only allow one running instance.\n\nYou can access this Setting under the menu item 'Environment'."),
                             "Code::Blocks", wxOK | wxICON_ERROR);
                return false;
            }
        }

        // Now we can start the DDE-/IPC-Server, if we did it earlier we would connect to ourselves
        if (m_DDE && !m_Batch)
        {
            g_DDEServer = new DDEServer(nullptr);
            g_DDEServer->Create(wxString::Format(DDE_SERVICE, wxGetUserId()));
        }

        if (!m_Batch)
            Manager::Get()->GetUserVariableManager()->SetUI(std::unique_ptr<UserVarManagerUI>(new UserVarManagerGUI()));

        // Splash screen moved to this place, otherwise it would be short visible, even if we only pass filenames via DDE/IPC
        // we also don't need it, if only a single instance is allowed
        Splash splash(!m_Batch && m_Script.IsEmpty() && m_Splash &&
                      appCfg->ReadBool("/environment/show_splash", true));
        InitDebugConsole();

        Manager::SetBatchBuild(m_Batch || !m_Script.IsEmpty());
        Manager::Get()->GetScriptingManager();
        MainFrame* frame = nullptr;
        frame = InitFrame();
        m_Frame = frame;

        {
            const double scalingFactor = cbGetContentScaleFactor(*frame);
            const double actualScalingFactor = cbGetActualContentScaleFactor(*frame);
            log->Log(wxString::Format(_("Initial scaling factor is %.3f (actual: %.3f)"),
                                      scalingFactor, actualScalingFactor));
        }

        // plugins loaded -> check command line arguments again
        delete wxMessageOutput::Set(new wxMessageOutputBest); // warn about unknown options
        if (ParseCmdLine(m_Frame) == 0)
        {
            if (appCfg->ReadBool("/environment/blank_workspace", true) == false)
                Manager::Get()->GetProjectManager()->LoadWorkspace();
        }

        if (m_SafeMode)
            wxLog::EnableLogging(true); // re-enable logging in safe-mode

        if (m_Batch)
        {
            Manager::SetAppStartedUp(true);

            // the compiler plugin might be waiting for this
            CodeBlocksEvent event(cbEVT_APP_STARTUP_DONE);
            Manager::Get()->ProcessEvent(event);

            Manager::Get()->RegisterEventSink(cbEVT_COMPILER_FINISHED, new cbEventFunctor<CodeBlocksApp, CodeBlocksEvent>(this, &CodeBlocksApp::OnBatchBuildDone));
            s_Loading = false;
            LoadDelayedFiles(frame);

            // The OnInit function should only start the application but do no heavy work
            // CallAfter will queue the function at the end of the event loop, so
            // OnInit is finished before the build process is started.
            // Starting the build process here will lead to crashes on linux
            CallAfter([this]() { this->BatchJob(); });

            return true;
        }

        if (!m_Script.IsEmpty())
        {
            s_Loading = false;
            LoaderBase* loader = Manager::Get()->GetFileManager()->Load(m_Script);

            if (loader->GetData())
                Manager::Get()->GetScriptingManager()->LoadBuffer(cbC2U(loader->GetData()));

            delete loader;
            frame->Close();
            return true;
        }

        CheckVersion();

        // run startup script
        const wxString startup = ConfigManager::LocateDataFile("startup.script",
                                                               sdScriptsUser | sdScriptsGlobal);
        if (!startup.empty())
        {
            ScriptingManager *scriptMgr = Manager::Get()->GetScriptingManager();
            if (!scriptMgr->LoadScript(startup))
                scriptMgr->DisplayErrors();
        }

        Manager::ProcessPendingEvents();

        // finally, show the app
        splash.Hide();
        SetTopWindow(frame);
        frame->Show();

        frame->StartupDone();

        frame->ShowTips(); // this func checks if the user wants tips, so no need to check here

        if (platform::windows)
            InitAssociations();

        s_Loading = false;

        LoadDelayedFiles(frame);
        AttachDebugger();
        Manager::Get()->GetProjectManager()->WorkspaceChanged();

        // all done
        Manager::SetAppStartedUp(true);

        CodeBlocksEvent event(cbEVT_APP_STARTUP_DONE);
        Manager::Get()->ProcessEvent(event);

        if (!m_crashReportName.empty())
            Manager::Get()->GetLogManager()->Log(wxString::Format(_("Setting the crash report file to: %s"), m_crashReportName));

        return true;
    }
    catch (cbException& exception)
    {
        exception.ShowErrorMessage();
    }
    catch (const char* message)
    {
        wxSafeShowMessage("Exception", cbC2U(message));
    }
    catch (...)
    {
        wxSafeShowMessage("Exception", "Unknown exception was raised. The application will terminate immediately...");
    }
    // if we reached here, return error
    return false;
}

int CodeBlocksApp::OnExit()
{
    if (wxTheClipboard->IsOpened())
    {
        wxTheClipboard->Flush();
        wxTheClipboard->Close();
    }

    if (g_DDEServer) delete g_DDEServer;

    if (m_pSingleInstance)
        delete m_pSingleInstance;

    // ultimate shutdown...
    Manager::Free();

    // WX docs say that this function's return value is ignored,
    // but we return our value anyway. It might not be ignored at some point...
    return m_Batch ? m_BatchExitCode : 0;
}

#ifdef __WXMSW__
    inline void EnableLFH()
    {
        typedef BOOL  (WINAPI *HeapSetInformation_t)(HANDLE, HEAP_INFORMATION_CLASS, PVOID, SIZE_T);
        typedef DWORD (WINAPI *GetProcessHeaps_t)(DWORD, PHANDLE);

        HINSTANCE kh = GetModuleHandle(TEXT("kernel32.dll"));
        HeapSetInformation_t  HeapSetInformation_func = (HeapSetInformation_t)  GetProcAddress(kh, "HeapSetInformation");
        GetProcessHeaps_t     GetProcessHeaps_func    = (GetProcessHeaps_t)     GetProcAddress(kh, "GetProcessHeaps");

        if (GetProcessHeaps_func && HeapSetInformation_func)
        {
            ULONG  HeapFragValue = 2;

            int n = GetProcessHeaps_func(0, 0);
            HANDLE *h = new HANDLE[n];
            GetProcessHeaps_func(n, h);

            for (int i = 0; i < n; ++i)
                HeapSetInformation_func(h[i], HeapCompatibilityInformation, &HeapFragValue, sizeof(HeapFragValue));

            delete[] h;
        }
    }
#else
    inline void EnableLFH() {}
#endif

int CodeBlocksApp::OnRun()
{
    EnableLFH();
    try
    {
        int retval = wxApp::OnRun();
        // wx 2.6.3 docs says that OnRun() function's return value is used as exit code
        return m_Batch ? m_BatchExitCode : retval;
    }
    catch (cbException& exception)
    {
        exception.ShowErrorMessage();
    }
    catch (const char* message)
    {
        wxSafeShowMessage(_("Exception"), cbC2U(message));
    }
    catch (...)
    {
        wxSafeShowMessage(_("Exception"), _("Unknown exception was raised. The application will terminate immediately..."));
    }
    // if we reached here, return error
    return -1;
}

bool CodeBlocksApp::OnCmdLineParsed(wxCmdLineParser& parser)
{
    return wxApp::OnCmdLineParsed(parser);
}

void CodeBlocksApp::OnFatalException()
{
#if wxUSE_DEBUGREPORT && wxUSE_XML && wxUSE_ON_FATAL_EXCEPTION
    wxDebugReport report;
    if (report.IsOk())
    {
        wxDebugReportPreviewStd preview;

        report.AddAll();
        if ( preview.Show(report) )
            report.Process();
    }
    else
    {
        cbMessageBox(wxString::Format(_("Debug report initialization failed, %s will terminate immediately.\n"
                                        "We are sorry for the inconvenience..."), appglobals::AppName));
    }
#else
    cbMessageBox(wxString::Format(_("Something has gone wrong inside %s and it will terminate immediately.\n"
                                    "We are sorry for the inconvenience..."), appglobals::AppName));
#endif
}

int CodeBlocksApp::BatchJob()
{
    if (!m_Batch)
        return -1;

    // find compiler plugin
    cbCompilerPlugin *compiler = Manager::Get()->GetPluginManager()->GetFirstCompiler();
    if (!compiler)
        return -3;

    if (!m_Clean && m_BatchTarget.Lower() == _T("ask"))
    {
        m_BatchTarget.Clear();
        cbProject* prj = Manager::Get()->GetProjectManager()->GetActiveProject();
        if (prj)
        {
            int idx = -1;
            wxString defTarget = prj->GetActiveBuildTarget();
            // find active target's index
            // TODO: make this easier in the SDK
            for (int i = 0; i < prj->GetBuildTargetsCount(); ++i)
            {
                ProjectBuildTarget* target = prj->GetBuildTarget(i);
                if ( target->GetTitle().Matches(defTarget) )
                {
                    idx = i;
                    break;
                }
            }
            idx = prj->SelectTarget(idx, false);
            if (idx == -1)
                return 0; // no target selected: just abort
            m_BatchTarget = prj->GetBuildTarget(idx)->GetTitle();
        }
    }

    m_pBatchBuildDialog = m_Frame->GetBatchBuildDialog();
    PlaceWindow(m_pBatchBuildDialog);

    const wxString title(wxString::Format(_("Building '%s' (target '%s')"), wxFileNameFromPath(wxString(argv[argc-1])), m_BatchTarget));
    wxTaskBarIcon* tbIcon = new wxTaskBarIcon();
    tbIcon->SetIcon(
            #ifdef __WXMSW__
                wxICON(A_MAIN_ICON),
            #else
                wxIcon(app_xpm),
            #endif // __WXMSW__
                title);

    const wxString bb_title(m_pBatchBuildDialog->GetTitle());
    m_pBatchBuildDialog->SetTitle(bb_title+" - "+title);
    m_pBatchBuildDialog->Show();
    // Clean up after the window is closed
    m_pBatchBuildDialog->Bind(wxEVT_CLOSE_WINDOW, &CodeBlocksApp::OnCloseBatchBuildWindow, this);


    if (m_ReBuild)
    {
        if (m_HasProject)
            compiler->Rebuild(m_BatchTarget);
        else if (m_HasWorkSpace)
            compiler->RebuildWorkspace(m_BatchTarget);
    }
    else if (m_Build)
    {
        if (m_HasProject)
            compiler->Build(m_BatchTarget);
        else if (m_HasWorkSpace)
            compiler->BuildWorkspace(m_BatchTarget);
    }
    else if (m_Clean)
    {
        if (m_HasProject)
            compiler->Clean(m_BatchTarget);
        else if (m_HasWorkSpace)
            compiler->CleanWorkspace(m_BatchTarget);
    }

    if (tbIcon)
    {
        tbIcon->RemoveIcon();
        delete tbIcon;
    }

    return 0;
}

void CodeBlocksApp::OnCloseBatchBuildWindow(wxCloseEvent& evt)
{
    cbCompilerPlugin *compiler = Manager::Get()->GetPluginManager()->GetFirstCompiler();
    if(compiler != nullptr && compiler->IsRunning())
    {
        if( cbMessageBox(_("Build still running. Do you want stop the build process?"), appglobals::AppName, wxICON_QUESTION | wxYES_NO, m_pBatchBuildDialog) == wxID_YES )
        {
            evt.Veto();
            compiler->KillProcess();
        }
    }
    else
    {
        m_Frame->Close();
    }
}

void CodeBlocksApp::OnBatchBuildDone(CodeBlocksEvent& event)
{
    event.Skip();
    // the event comes more than once. deal with it...
    static bool one_time_only = false;
    if (!m_Batch || one_time_only)
        return;
    one_time_only = true;

    cbCompilerPlugin* compiler = static_cast<cbCompilerPlugin*>(event.GetPlugin());
    m_BatchExitCode = compiler->GetExitCode();

    if (m_BatchNotify)
    {
        wxString msg;
        if (m_BatchExitCode == 0)
            msg << _("Batch build ended.\n");
        else
            msg << _("Batch build stopped with errors.\n");
        msg << wxString::Format(_("Process exited with status code %d."), m_BatchExitCode);
        cbMessageBox(msg, appglobals::AppName, m_BatchExitCode == 0 ? wxICON_INFORMATION : wxICON_WARNING, m_pBatchBuildDialog);
    }
    else
        wxBell();

    // Clean up happens in in the close handler of the window
    // We can not close the window here, because the origin of this event
    // is the compiler plugin and the plugin will write messages to the log window after this call
    // If we delete it here this will lead to memory corruption.
    // The solution is to queue the call to close the log window to the end
    // of the event loop with CallAfter. So the compiler plugin can finish its
    // work and we close the window afterwards.
    if (m_pBatchBuildDialog && m_BatchWindowAutoClose)
        CallAfter([this]() { m_pBatchBuildDialog->Close(); });

}

void CodeBlocksApp::OnTBIconLeftDown(wxTaskBarIconEvent& event)
{
    event.Skip();
    if (m_pBatchBuildDialog)
    {
        m_pBatchBuildDialog->Raise();
        m_pBatchBuildDialog->Refresh();
    }
}

wxString CodeBlocksApp::GetAppPath() const
{
    wxString base;
#ifdef __WXMSW__
    wxChar name[MAX_PATH] = {0};
    GetModuleFileName(0L, name, MAX_PATH);
    wxFileName fname(name);
    base = fname.GetPath(wxPATH_GET_VOLUME);
#else
    if (!m_Prefix.IsEmpty())
        return m_Prefix;

    base = wxStandardPaths::Get().GetExecutablePath();
    base = wxFileName(base).GetPath();
    if (base.empty())
        base = ".";
#endif
    return base;
}

void CodeBlocksApp::SetAutoFile(wxString& file)
{
    m_AutoFile = file;
}

int CodeBlocksApp::ParseCmdLine(MainFrame* handlerFrame, const wxString& CmdLineString,
                                const wxString &CWD)
{
    // code shamelessely taken from the console wxWindows sample :)
    bool filesInCmdLine = false;

#if wxUSE_CMDLINE_PARSER
    wxCmdLineParser& parser = *Manager::GetCmdLineParser();
    if ( CmdLineString.IsEmpty() )
        parser.SetCmdLine(argc, argv);
    else
        parser.SetCmdLine(CmdLineString);
    // wxApp::argc is a wxChar**

    // don't display errors as plugins will have the chance to parse the command-line
    // too, so we don't know here what exactly are the supported options
    int res = parser.Parse(false);
    if (res == -1)
    {
        return -1;
    }
    else
    {
        if (handlerFrame)
        {
            m_HasProject = false;
            m_HasWorkSpace = false;

            Manager::Get()->GetUserVariableManager()->ParseCommandLine(parser);

            int count = parser.GetParamCount();

            parser.Found(_T("file"), &m_AutoFile);

            if (!m_AutoFile.empty() && !CWD.empty())
            {
                wxFileName file(m_AutoFile);
                if (file.IsRelative())
                {
                    // Use the CurrentWorkingDirectory of the client instance to restore the
                    // absolute path to the file.
                    file.MakeAbsolute(CWD);
                    m_AutoFile = file.GetFullPath();
                }
            }

            filesInCmdLine = (count != 0) || (!m_AutoFile.empty());

            for (int param = 0; param < count; ++param)
            {
                const wxString &strParam = parser.GetParam(param);
                wxFileName fn(strParam);
                // Really important so that two same files with different names are not loaded
                // twice. Use the CurrentWorkingDirectory of the client instance to restore the
                // absolute path to the file.
                fn.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_TILDE | wxPATH_NORM_ABSOLUTE | wxPATH_NORM_LONG | wxPATH_NORM_SHORTCUT, CWD);
                const wxString &paramFullPath = fn.GetFullPath();

                // Is it a project/workspace?
                const FileType ft = FileTypeOf(strParam);
                if (ft == ftCodeBlocksProject)
                {
                    m_HasProject = true;
                    m_DelayedFilesToOpen.Add(paramFullPath);
                }
                else if (ft == ftCodeBlocksWorkspace)
                {
                    // only one workspace can be opened
                    m_HasWorkSpace = true;
                    m_DelayedFilesToOpen.Clear(); // remove all other files
                    m_DelayedFilesToOpen.Add(paramFullPath); // and add only the workspace
                    break; // and stop processing any more files
                }
                //else if (ft == ftSource || ft == ftHeader || ft == ftResource)
                else if (wxFile::Exists(paramFullPath)) //also try to open non source, header and resource files
                {
                    m_DelayedFilesToOpen.Add(paramFullPath);
                }
            }

            // batch jobs
            m_Batch = m_HasProject || m_HasWorkSpace;
            m_Batch = m_Batch && (m_Build || m_ReBuild || m_Clean);
        }
        else
        {
            wxString val;
            parser.Found(_T("prefix"), &m_Prefix);
            parser.Found(_T("user-data-dir"), &m_UserDataDir);
#ifdef __WXMSW__
            m_DDE = !parser.Found(_T("no-dde"));
            m_Assocs = !parser.Found(_T("no-check-associations"));
#else
            m_DDE = !parser.Found(_T("no-ipc"));
#endif
            m_SafeMode = parser.Found(_T("safe-mode"));
            m_Splash = !parser.Found(_T("no-splash-screen"));
            m_HasDebugLog = parser.Found(_T("debug-log"));
            m_CrashHandler = !parser.Found(_T("no-crash-handler"));

            wxLog::EnableLogging(parser.Found(_T("verbose")));

            if (   parser.Found(_T("personality"), &val)
                || parser.Found(_T("profile"),     &val) )
            {
                SetupPersonality(val);
            }

            // batch jobs
            m_BatchNotify          = parser.Found(_T("batch-build-notify"));
            m_BatchWindowAutoClose = !parser.Found(_T("no-batch-window-close"));
            m_Build                = parser.Found(_T("build"));
            m_ReBuild              = parser.Found(_T("rebuild"));
            m_Clean                = parser.Found(_T("clean"));
            parser.Found(_T("target"), &m_BatchTarget);
            parser.Found(_T("script"), &m_Script);
            // initial setting for batch flag (will be reset when ParseCmdLine() is called again).
            m_Batch = m_Build || m_ReBuild || m_Clean;


            if (parser.Found(_T("no-log")) == false)
                Manager::Get()->GetLogManager()->SetLog(new TextCtrlLogger, LogManager::app_log);
            if (parser.Found(_T("log-to-file")))
                Manager::Get()->GetLogManager()->SetLog(new FileLogger(_T("codeblocks.log")), LogManager::app_log);
            if (m_HasDebugLog)
                Manager::Get()->GetLogManager()->SetLog(new TextCtrlLogger, LogManager::debug_log);
            if (parser.Found(_T("debug-log-to-file")))
                Manager::Get()->GetLogManager()->SetLog(new FileLogger(_T("codeblocks-debug.log")), LogManager::debug_log);
        }

        // Always parse the debugger attach parameters.
        parser.Found(_T("dbg-attach"), &m_DebuggerAttach);
        parser.Found(_T("dbg-config"), &m_DebuggerConfig);
    }
#endif // wxUSE_CMDLINE_PARSER
    return filesInCmdLine ? 1 : 0;
}

void CodeBlocksApp::SetupPersonality(const wxString& personality)
{
    PersonalityManager *personalityMgr = Manager::Get()->GetPersonalityManager();

    if (personality.CmpNoCase(_T("ask")) == 0)
    {
        const wxArrayString items(personalityMgr->GetPersonalitiesList());

        wxSingleChoiceDialog dlg(nullptr, _("Please choose which personality (profile) to load:"),
                                          _("Personalities (profiles)"),
                                          items);
        if (dlg.ShowModal() == wxID_OK)
            personalityMgr->SetPersonality(dlg.GetStringSelection());
    }
    else
        personalityMgr->SetPersonality(personality, true);
}

void CodeBlocksApp::LoadDelayedFiles(MainFrame *const frame)
{
    std::set<wxString> uniqueFilesToOpen(m_DelayedFilesToOpen.begin(), m_DelayedFilesToOpen.end());
    for (std::set<wxString>::const_iterator it = uniqueFilesToOpen.begin(); it != uniqueFilesToOpen.end(); ++it)
        frame->Open(*it, true);
    m_DelayedFilesToOpen.Clear();

    // --file foo.cpp[:line]
    if (!m_AutoFile.IsEmpty())
    {
        wxString linePart;
        // We always want to open the file no matter if there is a line number or not.
        wxString filePart = m_AutoFile;
        long linePos = m_AutoFile.Find(_T(':'), true);
        if (linePos != wxNOT_FOUND)
        {
            linePart = m_AutoFile.Mid(linePos + 1, wxString::npos);
            filePart.Remove(linePos);
        }

        long line = -1;
        if (linePos != wxNOT_FOUND)
        {
            // on windows, if ":line" is omitted:
            // assuming drive letter before the colon if ToLong fails
            // c:\foo\bar.h gives \foo\bar.h
            if ( !linePart.ToLong(&line) )
            {
                // on windows, if :line is omitted: c:\foo\bar.h -> \foo\bar.h is not the line number!
                filePart = m_AutoFile;
            }
        }
        // Make sure filePart is not empty, because if it is empty Normalize turns the full path in
        // to the path of the current working folder.
        if (!filePart.empty())
        {
            wxFileName fn(filePart);
            // really important so that two same files with different names are not loaded twice
            fn.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_TILDE | wxPATH_NORM_ABSOLUTE | wxPATH_NORM_LONG | wxPATH_NORM_SHORTCUT);
            if (frame->Open(fn.GetFullPath(), false))
            {
                EditorBase* eb = Manager::Get()->GetEditorManager()->GetEditor(fn.GetFullPath());
                if (eb && (line != -1))
                    eb->GotoLine(line - 1, true);
            }
        }
        m_AutoFile.Clear();
    }
}

void CodeBlocksApp::AttachDebugger()
{
    const wxString localAttach = m_DebuggerAttach;
    const wxString localConfig = m_DebuggerConfig;
    // Reset the values to prevent old values to be used when the user forgets to pass all required
    // command line parameters.
    m_DebuggerAttach = m_DebuggerConfig = wxString();

    LogManager *logManager = Manager::Get()->GetLogManager();

    if (localAttach.empty() || localConfig.empty())
    {
        if (localAttach.empty() != localConfig.empty())
        {
            logManager->LogError(
                _("For attaching to work you need to provide both '--dbg-attach' and '--dbg-config'"));
            logManager->Log(wxT("    --dbg-attach='") + localAttach + wxT("'"));
            logManager->Log(wxT("    --dbg-config='") + localConfig + wxT("'"));
        }
        return;
    }

    logManager->Log(wxString::Format(_("Attach debugger '%s' to '%s'"), localConfig, localAttach));

    // Split the dbg-config to plugin name and config name
    wxString::size_type pos = localConfig.find(wxT(':'));
    if (pos == wxString::npos || pos == 0)
    {
        logManager->LogError(
            _("No delimiter found. The --dbg-config format is 'plugin-name:config-name'"));
        return;
    }

    const wxString pluginName = localConfig.substr(0, pos);
    const wxString configName = localConfig.substr(pos + 1);

    // Find the plugin and the config.
    DebuggerManager *debuggerManager = Manager::Get()->GetDebuggerManager();
    const DebuggerManager::RegisteredPlugins &debuggers = debuggerManager->GetAllDebuggers();
    if (debuggers.empty())
    {
        logManager->LogError(_("No debugger plugins loaded!"));
        return;
    }

    cbDebuggerPlugin *plugin = nullptr;
    int configIndex = -1;
    const DebuggerManager::PluginData *pluginData = nullptr;

    for (const auto &info : debuggers)
    {
        if (info.first->GetSettingsName() == pluginName)
        {
            plugin = info.first;
            pluginData = &info.second;
            break;
        }
    }

    if (!plugin)
    {
        logManager->LogError(wxString::Format(_("Debugger plugin '%s' not found!"),
                                              pluginName));
        logManager->Log(_("Available plugins:"));
        for (const auto &info : debuggers)
        {
            cbDebuggerPlugin *p = info.first;
            logManager->Log(wxString::Format("    '%s' (%s)", p->GetSettingsName(), p->GetGUIName()));
        }
        return;
    }

    const DebuggerManager::ConfigurationVector &configs = pluginData->GetConfigurations();
    for (auto it = configs.begin(); it != configs.end(); ++it)
    {
        if ((*it)->GetName() == configName)
        {
            configIndex = std::distance(configs.begin(), it);
            break;
        }
    }

    if (configIndex == -1)
    {
        logManager->LogError(wxString::Format(_("Debugger configuration '%s' not found!"),
                                              configName));
        logManager->Log(_("Available configurations:"));
        for (const cbDebuggerConfiguration *config : configs)
            logManager->Log(wxString::Format("    '%s'", config->GetName()));
        return;
    }

    // We have a debugger plugin and config, so lets try to attach...
    logManager->Log(_("Debugger plugin and configuration found. Attaching!!!"));
    plugin->SetActiveConfig(configIndex);
    plugin->AttachToProcess(localAttach);
}

#ifdef __WXMAC__

void CodeBlocksApp::MacOpenFile(const wxString & fileName )
{
    if (s_Loading)
        m_DelayedFilesToOpen.Add(fileName);
    else if (m_Frame)
        m_Frame->Open(fileName, true);
}

void CodeBlocksApp::MacPrintFile(const wxString & fileName )
{
    // TODO
    wxApp::MacPrintFile(fileName);
}

#endif // __WXMAC__

// event handlers

void CodeBlocksApp::OnAppActivate(wxActivateEvent& event)
{
    // allow others to process this event
    event.Skip();

    if (s_Loading)
        return; // still loading; we can't possibly be interested for this event ;)

    Manager *manager = Manager::Get();
    if (!manager || manager->IsAppShuttingDown())
        return;

    // Activation & De-Activation event
    CodeBlocksEvent cbEvent;
    if (event.GetActive())
        cbEvent.SetEventType(cbEVT_APP_ACTIVATED);
    else
        cbEvent.SetEventType(cbEVT_APP_DEACTIVATED);
    Manager::Get()->ProcessEvent(cbEvent);

    if (!event.GetActive())
        return;

    // fix for bug #18007: In batch build mode the following is not needed
    if (  !m_Batch
        && Manager::Get()->GetEditorManager()
        && Manager::Get()->GetConfigManager(_T("app"))->ReadBool(_T("/environment/check_modified_files"), true))
    {
        // for some reason a mouse up event doesn't make it into scintilla (scintilla bug)
        // therefore the workaround is not to directly call the editorManager, but
        // take a detour through an event
        // the bug is when the file has been offered to reload, no matter what answer you
        // give the mouse is in a selecting mode, adding/removing things to it's selection as you
        // move it around
        // so : idEditorManagerCheckFiles, EditorManager::OnCheckForModifiedFiles just exist for this workaround
        wxCommandEvent evt(wxEVT_COMMAND_MENU_SELECTED, idEditorManagerCheckFiles);
        wxPostEvent(Manager::Get()->GetEditorManager(), evt);
        // If event is close app, wxPostEvent() may have allowed OnApplicationClose() to free the Managers.
        if ( Manager::IsAppShuttingDown())
            return;
        cbProjectManagerUI *prjManUI = m_Frame->GetProjectManagerUI();
        if (prjManUI)
            static_cast<ProjectManagerUI*>(prjManUI)->CheckForExternallyModifiedProjects();
    }
    cbEditor* ed = Manager::Get()->GetEditorManager()
                 ? Manager::Get()->GetEditorManager()->GetBuiltinActiveEditor() : nullptr;
    if (ed)
    {
        // hack for linux: without it, the editor loses the caret every second activate o.O
        Manager::Get()->GetEditorManager()->GetNotebook()->SetFocus();
        ed->GetControl()->SetFocus();
    }
}

void CodeBlocksApp::AddFileToOpenDelayed(const wxString& filename)
{
    m_DelayedFilesToOpen.Add(filename);
}
