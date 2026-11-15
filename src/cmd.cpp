// curtimefix: the `curtimefix` console command. The only file built against the Source SDK headers.
// ConCommand without tier1's convar.cpp, the way CS:TV does it: the class (and its vtable) is exactly the SDK's,
// only the methods ConCommand needs are defined here.
#include "icvar.h"
#include "tier1/convar.h"

#include "ctf.h"

#ifdef _WIN32
#define PLAT_VSTDLIB "vstdlib"
#else
#define PLAT_VSTDLIB "libvstdlib"
#endif

static ICvar *g_cvar;
static CVarDLLIdentifier_t g_dll_id = -1;

ConCommandBase::ConCommandBase() : m_pNext(nullptr), m_bRegistered(false), m_pszName(nullptr), m_pszHelpString(nullptr), m_nFlags(0) {}
ConCommandBase::~ConCommandBase() {}
bool ConCommandBase::IsCommand() const { return true; }
bool ConCommandBase::IsFlagSet(int flag) const { return (flag & m_nFlags) != 0; }
void ConCommandBase::AddFlags(int flags) { m_nFlags |= flags; }
const char *ConCommandBase::GetName() const { return m_pszName; }
const char *ConCommandBase::GetHelpText() const { return m_pszHelpString; }
bool ConCommandBase::IsRegistered() const { return m_bRegistered; }
CVarDLLIdentifier_t ConCommandBase::GetDLLIdentifier() const { return g_dll_id; }
void ConCommandBase::CreateBase(const char *name, const char *help, int flags)
{
    m_bRegistered = false;
    m_pszName = name;
    m_pszHelpString = help ? help : "";
    m_nFlags = flags;
    m_pNext = nullptr;
}
void ConCommandBase::Init() {}
ConCommand::ConCommand(const char *name, FnCommandCallback_t cb, const char *help, int flags, FnCommandCompletionCallback)
{
    m_fnCommandCallback = cb;
    m_fnCompletionCallback = nullptr;
    m_bHasCompletionCallback = false;
    m_bUsingNewCommandCallback = true;
    m_bUsingCommandCallbackInterface = false;
    CreateBase(name, help, flags);
}
ConCommand::~ConCommand() {}
bool ConCommand::IsCommand() const { return true; }
int ConCommand::AutoCompleteSuggest(const char *, CUtlVector<CUtlString> &) { return 0; }
bool ConCommand::CanAutoComplete() { return false; }
void ConCommand::Dispatch(const CCommand &command) { m_fnCommandCallback(command); }

static void ctf_cb(const CCommand &args)
{
    const char *argv[8];
    int argc = args.ArgC() < 8 ? args.ArgC() : 8;
    for (int i = 0; i < argc; i++) argv[i] = args[i];
    ctf_command(argc, argv);
}

static ConCommand g_cmd("curtimefix", ctf_cb, "curtimefix: no argument = status, 0/1 = off/on (A/B), meter, selftest, reset");

bool cmd_register()
{
    g_cvar = (ICvar *)plat_interface(PLAT_VSTDLIB, CVAR_INTERFACE_VERSION);
    if (!g_cvar) return false;
    g_dll_id = g_cvar->AllocateDLLIdentifier();
    g_cvar->RegisterConCommand(&g_cmd);
    return true;
}
