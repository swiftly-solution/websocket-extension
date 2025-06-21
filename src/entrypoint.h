#ifndef _entrypoint_h
#define _entrypoint_h

#include <string>

#include <swiftly-ext/core.h>
#include <swiftly-ext/extension.h>
#include <swiftly-ext/hooks/function.h>
#include <swiftly-ext/hooks/vfunction.h>

#include <deque>
#include <any>
#include <vector>

class WSExtension : public SwiftlyExt
{
public:
    bool Load(std::string& error, SourceHook::ISourceHook* SHPtr, ISmmAPI* ismm, bool late);
    bool Unload(std::string& error);

    void AllExtensionsLoaded();
    void AllPluginsLoaded();

    bool OnPluginLoad(std::string pluginName, void* pluginState, PluginKind_t kind, std::string& error);
    bool OnPluginUnload(std::string pluginName, void* pluginState, PluginKind_t kind, std::string& error);

    void Hook_PreWorldUpdate(bool simulating);

public:
    const char* GetAuthor();
    const char* GetName();
    const char* GetVersion();
    const char* GetWebsite();

    void NextFrame(std::function<void(std::vector<std::any>)> fn, std::vector<std::any> param);

private:
    std::deque<std::pair<std::function<void(std::vector<std::any>)>, std::vector<std::any>>> m_nextFrame;
};

template <typename... Args>
std::string string_format(const std::string& format, Args... args)
{
    int size_s = snprintf(nullptr, 0, format.c_str(), args...) + 1;
    if (size_s <= 0)
        return "";

    size_t size = static_cast<size_t>(size_s);
    char* buf = new char[size];
    snprintf(buf, size, format.c_str(), args...);
    std::string out = std::string(buf, buf + size - 1);
    delete buf;
    return out;
}

extern WSExtension g_Ext;
extern ISource2Server* server;
DECLARE_GLOBALVARS();

#endif