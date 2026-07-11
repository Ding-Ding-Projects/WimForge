#include "startup/Elevation.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <string_view>

namespace {

std::wstring quoteWindowsArgument(const std::wstring_view argument)
{
    if (!argument.empty()
        && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }

    std::wstring quoted(1, L'"');
    std::size_t backslashCount = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashCount;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashCount * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashCount, L'\\');
            quoted.push_back(character);
        }
        backslashCount = 0;
    }
    quoted.append(backslashCount * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring currentExecutablePath(unsigned long *error)
{
    std::wstring path(1024, L'\0');
    while (path.size() <= 32768) {
        const DWORD length = ::GetModuleFileNameW(
            nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            *error = ::GetLastError();
            return {};
        }
        if (static_cast<std::size_t>(length) < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
    *error = ERROR_INSUFFICIENT_BUFFER;
    return {};
}

std::wstring currentParameters(unsigned long *error)
{
    int argumentCount = 0;
    wchar_t **arguments = ::CommandLineToArgvW(::GetCommandLineW(), &argumentCount);
    if (!arguments) {
        *error = ::GetLastError();
        return {};
    }

    std::wstring parameters;
    for (int index = 1; index < argumentCount; ++index) {
        if (!parameters.empty())
            parameters.push_back(L' ');
        parameters += quoteWindowsArgument(arguments[index]);
    }
    ::LocalFree(arguments);
    return parameters;
}

} // namespace
#endif

namespace wimforge::startup {

ElevationResult ensureElevated()
{
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return {ElevationAction::Failed, ::GetLastError(),
                ElevationFailureStage::OpenProcessToken};

    TOKEN_ELEVATION elevation{};
    DWORD returnedLength = 0;
    const BOOL queried = ::GetTokenInformation(token, TokenElevation, &elevation,
                                               sizeof(elevation), &returnedLength);
    const DWORD tokenError = queried ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(token);
    if (!queried)
        return {ElevationAction::Failed, tokenError,
                ElevationFailureStage::QueryTokenElevation};
    if (elevation.TokenIsElevated != 0)
        return {ElevationAction::Continue, ERROR_SUCCESS,
                ElevationFailureStage::None};

    unsigned long error = ERROR_SUCCESS;
    const std::wstring executable = currentExecutablePath(&error);
    if (executable.empty())
        return {ElevationAction::Failed, error,
                ElevationFailureStage::ResolveExecutable};
    const std::wstring parameters = currentParameters(&error);
    if (error != ERROR_SUCCESS)
        return {ElevationAction::Failed, error,
                ElevationFailureStage::ParseArguments};

    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS;
    launch.lpVerb = L"runas";
    launch.lpFile = executable.c_str();
    launch.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!::ShellExecuteExW(&launch))
        return {ElevationAction::Failed, ::GetLastError(),
                ElevationFailureStage::RequestRelaunch};
    if (launch.hProcess)
        ::CloseHandle(launch.hProcess);
    return {ElevationAction::Relaunched, ERROR_SUCCESS,
            ElevationFailureStage::None};
#else
    return {ElevationAction::Continue, 0, ElevationFailureStage::None};
#endif
}

} // namespace wimforge::startup
