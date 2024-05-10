// SPDX-FileCopyrightText: Copyright (c) 2020-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//

#define __STDC_FORMAT_MACROS 1
#define _CRT_NONSTDC_NO_WARNINGS
#include <OmniClient.h>
#include <OmniUsdResolver.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <condition_variable>
#include <string>
#include <vector>

#include <pxr/pxr.h>
#include <pxr/usd/usd/stage.h>

static const int MAX_URL_SIZE = 2048;

bool g_shutdown = false;
bool g_haveUpdates = false;
std::mutex g_mutex;
std::condition_variable g_cv;
PXR_NS::UsdStageRefPtr g_stage;
OmniClientRequestId g_channel = 0;

template<class Mutex>
auto make_lock(Mutex& m)
{
    return std::unique_lock<Mutex>(m);
}

using ArgVec = std::vector<std::string>;

bool iequal(std::string const& a, std::string const& b)
{
    return (a.size() == b.size()) &&
        std::equal(a.begin(), a.end(), b.begin(),
            [](auto aa, auto bb)
            {
                return tolower(aa) == tolower(bb);
            });
}

/*
Tokenize a line using the Windows command line rules:
* Arguments are delimited by white space, which is either a space or a tab.
* A string surrounded by double quotation marks is interpreted as a single argument,
	regardless of white space contained within. A quoted string can be embedded in an argument.
* A double quotation mark preceded by a backslash, \", is interpreted as a literal double quotation mark (").
* Backslashes are interpreted literally, unless they immediately precede a double quotation mark.
* If an even number of backslashes is followed by a double quotation mark, then one backslash (\) is placed in
	the argv array for every pair of backslashes (\\), and the double quotation mark (") is interpreted as a string
delimiter.
* If an odd number of backslashes is followed by a double quotation mark, then one backslash (\) is placed in the argv
array for every pair of backslashes (\\) and the double quotation mark is interpreted as an escape sequence by the
remaining backslash, causing a literal double quotation mark (") to be placed in argv.
*/
std::vector<std::string> tokenize(char const* line)
{
    std::vector<std::string> tokens;
    std::string token;
    bool inWhiteSpace = true;
    bool inQuote = false;
    uint32_t backslashCount = 0;
    for (char const* p = line; *p; p++)
    {
        if (*p == '\n')
        {
            break;
        }
        if (inWhiteSpace)
        {
            if (*p == ' ' || *p == '\t')
            {
                continue;
            }
            else
            {
                inWhiteSpace = false;
            }
        }
        if (*p == '\\')
        {
            backslashCount++;
            continue;
        }
        if (*p == '\"')
        {
            token.append(backslashCount / 2, '\\');
            if (backslashCount % 2 == 1)
            {
                token.push_back('\"');
            }
            else
            {
                inQuote = !inQuote;
            }
            backslashCount = 0;
            continue;
        }
        if (backslashCount > 0)
        {
            token.append(backslashCount, '\\');
            backslashCount = 0;
        }
        if (!inQuote && (*p == ' ' || *p == '\t'))
        {
            tokens.emplace_back(std::move(token));
            token.clear();
            inWhiteSpace = true;
            continue;
        }
        token.push_back(*p);
    }
    if (backslashCount > 0)
    {
        token.append(backslashCount, '\\');
    }
    if (!token.empty())
    {
        tokens.emplace_back(std::move(token));
    }
    return tokens;
}

int resultToRetcode(OmniClientResult result)
{
    switch (result)
    {
    case eOmniClientResult_Ok:
    case eOmniClientResult_OkLatest:
    case eOmniClientResult_OkNotYetFound:
        return EXIT_SUCCESS;
    default:
        return EXIT_FAILURE;
    }
}

int help(ArgVec const& args);

int noop(ArgVec const&)
{
    return EXIT_SUCCESS;
}

void printResult(OmniClientResult result)
{
    printf("%s\n", omniClientGetResultString(result));
}

char const* formatTime(uint64_t tns)
{
    time_t time = (time_t)(tns / 1'000'000'000);
    static char timeStr[30];
    strftime(timeStr, sizeof(timeStr), "%F %T", localtime(&time));
    return timeStr;
}

std::string getAccessString(uint16_t access)
{
    std::string accessString;
    if (access & fOmniClientAccess_Read)
    {
        if (!accessString.empty())
        {
            accessString += ", ";
        }
        accessString += "Read";
    }
    if (access & fOmniClientAccess_Write)
    {
        if (!accessString.empty())
        {
            accessString += ", ";
        }
        accessString += "Write";
    }
    if (access & fOmniClientAccess_Admin)
    {
        if (!accessString.empty())
        {
            accessString += ", ";
        }
        accessString += "Admin";
    }
    if (accessString.empty())
    {
        accessString = "None";
    }
    return accessString;
}

void printListEntries(uint32_t numEntries, struct OmniClientListEntry const* entries)
{
    size_t longestOwner = 0;
    size_t longestType = 0;
    std::vector<std::string> types;
    for (uint32_t i = 0; i < numEntries; i++)
    {
        if (entries[i].createdBy != nullptr)
        {
            longestOwner = std::max(longestOwner, std::string(entries[i].createdBy).length());
        }
        std::string type;
        {
            auto flags = entries[i].flags;
            if (flags & fOmniClientItem_IsInsideMount)
            {
                type.append("mounted ");
            }
            if (flags & fOmniClientItem_ReadableFile)
            {
                auto size = entries[i].size;
                auto sizeSuffix = "B ";
                if (size > 1000 * 1000 * 1000)
                {
                    size /= 1000 * 1000 * 1000;
                    sizeSuffix = "GB ";
                }
                else if (size > 1000 * 1000)
                {
                    size /= 1000 * 1000;
                    sizeSuffix = "MB ";
                }
                else if (size > 1000)
                {
                    size /= 1000;
                    sizeSuffix = "KB ";
                }
                type.append(std::to_string(size));
                type.append(sizeSuffix);

                if ((flags & fOmniClientItem_WriteableFile) == 0)
                {
                    type.append("read-only ");
                }
                type.append("file ");
            }
            if (flags & fOmniClientItem_IsMount)
            {
                type.append("mount-point ");
            }
            else if (flags & fOmniClientItem_CanHaveChildren)
            {
                if (flags & fOmniClientItem_DoesNotHaveChildren)
                {
                    type.append("empty-folder ");
                }
                else
                {
                    type.append("folder ");
                }
            }
            if (flags & fOmniClientItem_CanLiveUpdate)
            {
                type.append("live ");
            }
            if (flags & fOmniClientItem_IsOmniObject)
            {
                type.append("omni-object ");
            }
            if (flags & fOmniClientItem_IsChannel)
            {
                type.append("channel ");
            }
            if (flags & fOmniClientItem_IsCheckpointed)
            {
                type.append("checkpointed ");
            }
        }
        longestType = std::max(longestType, type.size());
        types.emplace_back(std::move(type));
    }
    for (uint32_t i = 0; i < numEntries; i++)
    {
        auto ownerStr = entries[i].createdBy;
        if (ownerStr == nullptr)
        {
            ownerStr = "";
        }
        //     Date, type/size, owner, relative name
        printf("%18s  %*s %*s %s %s\n", formatTime(entries[i].modifiedTimeNs), (int)longestType, types[i].c_str(), (int)longestOwner, ownerStr, entries[i].relativePath,
            entries[i].comment);
    }
}

int list(ArgVec const& args)
{
    const char* url = ".";
    if (args.size() > 1)
    {
        url = args[1].data();
    }
    int retCode = EXIT_FAILURE;
    omniClientReconnect(url);
    omniClientWait(omniClientList(url, &retCode,
        [](void* userData, OmniClientResult result, uint32_t numEntries, struct OmniClientListEntry const* entries) noexcept
        {
            *(int*)userData = resultToRetcode(result);
            if (result != eOmniClientResult_Ok)
            {
                printResult(result);
                return;
            }
            printListEntries(numEntries, entries);
        }));
    return retCode;
}

int stat(ArgVec const& args)
{
    const char* url = ".";
    if (args.size() > 1)
    {
        url = args[1].data();
    }
    int retCode = EXIT_FAILURE;
    omniClientReconnect(url);
    omniClientWait(omniClientStat(url, &retCode,
        [](void* userData, OmniClientResult result, struct OmniClientListEntry const* entry) noexcept
        {
            *(int*)userData = resultToRetcode(result);
            if (result != eOmniClientResult_Ok)
            {
                printResult(result);
                return;
            }
            printf("Access: %s\n", getAccessString(entry->access).c_str());
            {
                std::string type;
                auto flags = entry->flags;
                if (flags & fOmniClientItem_ReadableFile)
                {
                    type.append("ReadableFile ");
                }
                if (flags & fOmniClientItem_WriteableFile)
                {
                    type.append("WritableFile ");
                }
                if (flags & fOmniClientItem_IsInsideMount)
                {
                    type.append("IsInsideMount ");
                }
                if (flags & fOmniClientItem_IsMount)
                {
                    type.append("IsMount ");
                }
                if (flags & fOmniClientItem_CanHaveChildren)
                {
                    type.append("CanHaveChildren ");
                }
                if (flags & fOmniClientItem_DoesNotHaveChildren)
                {
                    type.append("DoesNotHaveChildren ");
                }
                if (flags & fOmniClientItem_CanLiveUpdate)
                {
                    type.append("CanLiveUpdate ");
                }
                if (flags & fOmniClientItem_IsOmniObject)
                {
                    type.append("IsOmniObject ");
                }
                if (flags & fOmniClientItem_IsChannel)
                {
                    type.append("IsChannel ");
                }
                if (flags & fOmniClientItem_IsCheckpointed)
                {
                    type.append("IsCheckpointed");
                }
                printf("Flags: %s\n", type.c_str());
            }
            printf("Size: %" PRIu64 "\n", entry->size);
            printf("Modified: %s by %s\n", formatTime(entry->modifiedTimeNs), entry->modifiedBy ? entry->modifiedBy : "Unknown");
            printf("Created: %s by %s\n", formatTime(entry->createdTimeNs), entry->createdBy ? entry->createdBy : "Unknown");
            printf("Version: %s\n", entry->version ? entry->version : "Not Supported");
            printf("Hash: %s\n", entry->hash ? entry->hash : "Not Supported");
        }));
    return retCode;
}

std::string combineWithBaseUrl(const char* path)
{
    std::string combined;
    size_t bufferSize = 100;
    combined.resize(bufferSize);
    bool success;
    do
    {
        success = (omniClientCombineWithBaseUrl(path, &combined[0], &bufferSize) != nullptr);
        combined.resize(bufferSize - 1);  // includes null terminator
    } while (!success);
    return combined;
}

int cd(ArgVec const& args, bool push)
{
    if (args.size() == 1)
    {
        auto combined = combineWithBaseUrl(".");
        printf("%s\n", combined.c_str());
        return EXIT_SUCCESS;
    }
    std::string arg{ args[1] };
    if (arg.empty() || arg.back() != '/')
    {
        arg.push_back('/');
    }
    auto combined = combineWithBaseUrl(arg.c_str());
    if (combined.empty())
    {
        // I'm actually not sure if this can ever fail
        printf("Invalid value %s\n", args[1].data());
        return EXIT_FAILURE;
    }
    if (!push)
    {
        omniClientPopBaseUrl(nullptr);
    }
    omniClientPushBaseUrl(combined.c_str());
    printf("%s\n", combined.c_str());
    return EXIT_SUCCESS;
}

int cd(ArgVec const& args)
{
    return cd(args, false);
}

int push(ArgVec const& args)
{
    return cd(args, true);
}

int pop(ArgVec const&)
{
    omniClientPopBaseUrl(nullptr);
    return EXIT_SUCCESS;
}

int logLevel(ArgVec const& args)
{
    if (args.size() > 1)
    {
        if (iequal(args[1], "debug") || iequal(args[1], "d"))
        {
            omniClientSetLogLevel(eOmniClientLogLevel_Debug);
            return EXIT_SUCCESS;
        }
        if (iequal(args[1], "verbose") || iequal(args[1], "v"))
        {
            omniClientSetLogLevel(eOmniClientLogLevel_Verbose);
            return EXIT_SUCCESS;
        }
        if (iequal(args[1], "info") || iequal(args[1], "i"))
        {
            omniClientSetLogLevel(eOmniClientLogLevel_Info);
            return EXIT_SUCCESS;
        }
        if (iequal(args[1], "warning") || iequal(args[1], "w"))
        {
            omniClientSetLogLevel(eOmniClientLogLevel_Warning);
            return EXIT_SUCCESS;
        }
        if (iequal(args[1], "error") || iequal(args[1], "e"))
        {
            omniClientSetLogLevel(eOmniClientLogLevel_Error);
            return EXIT_SUCCESS;
        }
    }
    printf("Valid log levels are: debug, verbose, info, warning, error\n");
    return EXIT_FAILURE;
}

int copy(ArgVec const& args)
{
    if (args.size() <= 2)
    {
        printf("Not enough arguments\n");
        return EXIT_FAILURE;
    }
    int retCode = EXIT_FAILURE;
    omniClientReconnect(args[1].data());
    omniClientReconnect(args[2].data());
    omniClientWait(omniClientCopy(
        args[1].data(),  // srcUrl
        args[2].data(),  // dstUrl
        &retCode,        // userData
        [](void* userData, OmniClientResult result) noexcept
        {  // callback
            *(int*)userData = resultToRetcode(result);
            printResult(result);
        },
        eOmniClientCopy_Overwrite  // overwrite behavior
        ));
    return retCode;
}

int move(ArgVec const& args)
{
    if (args.size() <= 2)
    {
        printf("Not enough arguments\n");
        return EXIT_FAILURE;
    }
    int retCode = EXIT_FAILURE;
    omniClientReconnect(args[1].data());
    omniClientReconnect(args[2].data());
    omniClientWait(omniClientMove(
        args[1].data(),  // srcUrl
        args[2].data(),  // dstUrl
        &retCode,        // userData
        [](void* userData, OmniClientResult result, bool copied) noexcept
        {  // callback
            *(int*)userData = resultToRetcode(result);
            if (result == eOmniClientResult_Ok)
            {
                if (copied)
                {
                    printf("Ok - copied and deleted source\n");
                }
                else
                {
                    printf("Ok - moved\n");
                }
            }
            else
            {
                if (copied)
                {
                    printf("Copied, but received %s when deleting source\n", omniClientGetResultString(result));
                }
                else
                {
                    printResult(result);
                }
            }
        },
        eOmniClientCopy_Overwrite  // overwrite behavior
        ));
    return retCode;
}

int del(ArgVec const& args)
{
    if (args.size() <= 1)
    {
        printf("Not enough arguments\n");
        return EXIT_FAILURE;
    }
    int retCode = EXIT_FAILURE;
    omniClientReconnect(args[1].data());
    omniClientWait(omniClientDelete(args[1].data(), &retCode,
        [](void* userData, OmniClientResult result) noexcept
        {
            *(int*)userData = resultToRetcode(result);
            printResult(result);
        }));
    return retCode;
}

int mkdir(ArgVec const& args)
{
    if (args.size() <= 1)
    {
        printf("Not enough arguments\n");
        return EXIT_FAILURE;
    }
    int retCode = EXIT_FAILURE;
    omniClientReconnect(args[1].data());
    omniClientWait(omniClientCreateFolder(args[1].data(), &retCode,
        [](void* userData, OmniClientResult result) noexcept
        {
            *(int*)userData = resultToRetcode(result);
            printResult(result);
        }));
    return retCode;
}

int cat(ArgVec const& args)
{
    if (args.size() <= 1)
    {
        printf("Not enough arguments\n");
        return EXIT_FAILURE;
    }
    int retCode = EXIT_FAILURE;
    omniClientReconnect(args[1].data());
    omniClientWait(omniClientReadFile(args[1].data(), &retCode,
        [](void* userData, OmniClientResult result, const char* /* version */, OmniClientContent* content) noexcept
        {
            *(int*)userData = resultToRetcode(result);
            if (result != eOmniClientResult_Ok)
            {
                printResult(result);
                return;
            }
            if (content->size > 1 * 1000 * 1000)
            {
                printf("File too large to cat: %zu bytes.\n", content->size);
                return;
            }
            bool isAscii = true;
            uint8_t* buffer = (uint8_t*)content->buffer;
            for (size_t i = 0; i < content->size; i++)
            {
                if (buffer[i] == '\n' || buffer[i] == '\t' || buffer[i] == '\r')
                {
                }
                else if (buffer[i] >= 127 || buffer[i] < 32)
                {
                    isAscii = false;
                    break;
                }
            }
            if (isAscii)
            {
                printf("%.*s\n", (int)content->size, buffer);
            }
            else
            {
                uint32_t* buffer2 = (uint32_t*)content->buffer;
                for (size_t i = 0; i < content->size; i += 4)
                {
                    auto sep = (i % 32 == 0) ? '\n' : ' ';
                    printf("%8x%c", buffer2[i / 4], sep);
                }
                printf("\n");
            }
        }));
    return retCode;
}

int clientVersion(ArgVec const&)
{
    printf("%s\n", omniClientGetVersionString());
    return EXIT_SUCCESS;
}

int resolverVersion(ArgVec const&)
{
    printf("%s\n", omniUsdResolverGetVersionString());
    return EXIT_SUCCESS;
}

int serverVersion(ArgVec const& args)
{
    const char* url = ".";
    if (args.size() > 1)
    {
        url = args[1].data();
    }
    else
    {
        printf("Not enough arguments: must provide an omniverse server URL (omniverse://server-name)\n");
        return EXIT_FAILURE;
    }
    int retCode = EXIT_FAILURE;
    omniClientReconnect(url);
    omniClientWait(omniClientGetServerInfo(url, &retCode,
        [](void* userData, OmniClientResult result, OmniClientServerInfo const* info) noexcept
        {
            *(int*)userData = resultToRetcode(result);
            if (result != eOmniClientResult_Ok)
            {
                printResult(result);
                return;
            }
            printf("%s\n", info->version);
        }));
    return retCode;
}

int loadUsd(ArgVec const& args)
{
    if (args.size() <= 1)
    {
        printf("Not enough arguments\n");
        return EXIT_FAILURE;
    }
    auto lock = make_lock(g_mutex);
    g_stage = PXR_NS::UsdStage::Open(args[1].data());
    if (!g_stage)
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int saveUsd(ArgVec const& args)
{
    if (!g_stage)
    {
        printf("No USD loaded\n");
        return EXIT_FAILURE;
    }
    auto lock = make_lock(g_mutex);
    PXR_NS::TfErrorMark errorMark;
    errorMark.SetMark();
    if (args.size() <= 1)
    {
        g_stage->Save();
    }
    else if (!g_stage->Export(args[1].data()))
    {
        return EXIT_FAILURE;
    }
    if (errorMark.IsClean())
    {
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}

int closeUsd(ArgVec const&)
{
    if (!g_stage)
    {
        printf("No USD loaded\n");
        return EXIT_FAILURE;
    }
    g_stage = nullptr;
    return EXIT_SUCCESS;
}

int getacls(ArgVec const& args)
{
    const char* url = ".";
    if (args.size() > 1)
    {
        url = args[1].data();
    }
    int retCode = EXIT_FAILURE;
    omniClientReconnect(url);
    omniClientWait(omniClientGetAcls(url, &retCode,
        [](void* userData, OmniClientResult result, uint32_t numEntries, struct OmniClientAclEntry* entries) noexcept
        {
            *(int*)userData = resultToRetcode(result);
            if (result != eOmniClientResult_Ok)
            {
                printResult(result);
                return;
            }
            for (uint32_t i = 0; i < numEntries; i++)
            {
                printf("%s: %s\n", entries[i].name, getAccessString(entries[i].access).c_str());
            }
        }));
    return retCode;
}

int setacls(ArgVec const& args)
{
    if (args.size() <= 3)
    {
        printf("Not enough arguments\n");
        return EXIT_FAILURE;
    }
    char const* url = args[1].data();
    char const* name = args[2].data();
    char const* accessStr = args[3].data();

    bool removeEntry = false;
    uint16_t access = 0;

    for (char const* p = accessStr; *p; p++)
    {
        switch (toupper(*p))
        {
        case 'A':
            access |= fOmniClientAccess_Admin;
            break;
        case 'W':
            access |= fOmniClientAccess_Write;
            break;
        case 'R':
            access |= fOmniClientAccess_Read;
            break;
        case '-':
            removeEntry = true;
            break;
        default:
            printf("Unknown access \"%s\"\n", accessStr);
            return EXIT_FAILURE;
        }
    }
    struct GetAclsResult
    {
        OmniClientResult result;
        std::vector<std::pair<std::string, uint16_t>> entries;
    };
    GetAclsResult getAclsResult;

    omniClientReconnect(url);
    omniClientWait(omniClientGetAcls(url, &getAclsResult,
        [](void* userData, OmniClientResult result, uint32_t numEntries, struct OmniClientAclEntry* entries) noexcept
        {
            GetAclsResult& getAclsResultRef = *(GetAclsResult*)userData;
            getAclsResultRef.result = result;
            getAclsResultRef.entries.resize(numEntries);
            for (uint32_t i = 0; i < numEntries; i++)
            {
                getAclsResultRef.entries[i] = std::make_pair(entries[i].name, entries[i].access);
            }
        }));

    if (getAclsResult.result != eOmniClientResult_Ok)
    {
        printResult(getAclsResult.result);
        return EXIT_FAILURE;
    }

    std::vector<OmniClientAclEntry> entries;
    bool found = false;
    for (auto it = getAclsResult.entries.begin(); it != getAclsResult.entries.end(); ++it)
    {
        if (it->first == name)
        {
            // Is it possible for there to be 2 entries for the same user?
            // I have no idea, but I'll assume it is...
            found = true;
        }
        else
        {
            entries.push_back(OmniClientAclEntry{ it->first.c_str(), it->second });
        }
    }
    if (removeEntry)
    {
        if (!found)
        {
            printf("%s not in the ACLs list\n", name);
            return EXIT_FAILURE;
        }
    }
    else
    {
        entries.push_back(OmniClientAclEntry{ name, access });
    }

    int retCode = EXIT_FAILURE;
    omniClientWait(omniClientSetAcls(url, entries.size(), entries.data(), &retCode,
        [](void* userData, OmniClientResult result) noexcept
        {
            *(int*)userData = resultToRetcode(result);
            if (result != eOmniClientResult_Ok)
            {
                printResult(result);
                return;
            }
        }));
    return retCode;
}

int auth(ArgVec const& args)
{
    const char* user = "";
    const char* pass = "";

    if (args.size() > 1)
    {
        user = args[1].data();
        pass = user;
    }
    if (args.size() > 2)
    {
        pass = args[2].data();
    }

    {
        static char user_env[2000];
        snprintf(user_env, sizeof(user_env), "OMNI_USER=%s", user);
        putenv(user_env);
    }
    {
        static char pass_env[2000];
        snprintf(pass_env, sizeof(pass_env), "OMNI_PASS=%s", pass);
        putenv(pass_env);
    }

    return EXIT_SUCCESS;
}

int makeCheckpoint(ArgVec const& args)
{
    if (args.size() <= 1)
    {
        printf("Not enough arguments\n");
        return EXIT_FAILURE;
    }
    auto comment = (args.size() > 2) ? args[2].data() : "";
    int retCode = EXIT_FAILURE;
    omniClientReconnect(args[1].data());
    bool bForce = true;
    omniClientWait(omniClientCreateCheckpoint(args[1].data(), comment, bForce, &retCode,
        [](void* userData, OmniClientResult result, char const* checkpointQuery) noexcept
        {
            *(int*)userData = resultToRetcode(result);
            if (result != eOmniClientResult_Ok)
            {
                printResult(result);
                return;
            }
            printf("Checkpoint created as %s\n", checkpointQuery);
        }));
    return retCode;
}

int listCheckpoints(ArgVec const& args)
{
    if (args.size() <= 1)
    {
        printf("Not enough arguments\n");
        return EXIT_FAILURE;
    }
    int retCode = EXIT_FAILURE;
    omniClientReconnect(args[1].data());
    omniClientWait(omniClientListCheckpoints(args[1].data(), &retCode,
        [](void* userData, OmniClientResult result, uint32_t numEntries, struct OmniClientListEntry const* entries) noexcept
        {
            *(int*)userData = resultToRetcode(result);
            if (result != eOmniClientResult_Ok)
            {
                printResult(result);
                return;
            }
            printListEntries(numEntries, entries);
        }));
    return retCode;
}

int restoreCheckpoint(ArgVec const& args)
{
    if (args.size() <= 1)
    {
        printf("Not enough arguments\n");
        return EXIT_FAILURE;
    }
    char combined[MAX_URL_SIZE] = {};
    auto combinedSize = sizeof(combined);
    if (!omniClientCombineWithBaseUrl(args[1].data(), combined, &combinedSize))
    {
        printf("URL too long\n");
        return EXIT_FAILURE;
    }
    auto brokenUrl = omniClientBreakUrl(combined);
    auto branchCheckpoint = omniClientGetBranchAndCheckpointFromQuery(brokenUrl->query);
    if (branchCheckpoint == nullptr)
    {
        printf("No checkpoint provided\n");
        omniClientFreeUrl(brokenUrl);
        return EXIT_FAILURE;
    }
    // Re-build the query string with only the branch
    char queryBuffer[MAX_URL_SIZE];
    auto queryBufferSize = sizeof(queryBuffer);
    auto queryString = omniClientMakeQueryFromBranchAndCheckpoint(branchCheckpoint->branch, 0, queryBuffer, &queryBufferSize);
    if (queryString != queryBuffer)
    {
        printf("Query string buffer overflow\n");
        omniClientFreeBranchAndCheckpoint(branchCheckpoint);
        omniClientFreeUrl(brokenUrl);
        return EXIT_FAILURE;
    }
    brokenUrl->query = queryString;
    char dstUrlBuffer[MAX_URL_SIZE];
    auto dstUrlSize = sizeof(dstUrlBuffer);
    auto dstUrl = omniClientMakeUrl(brokenUrl, dstUrlBuffer, &dstUrlSize);
    if (dstUrl != dstUrlBuffer)
    {
        printf("URL buffer overflow\n");
        omniClientFreeBranchAndCheckpoint(branchCheckpoint);
        omniClientFreeUrl(brokenUrl);
        return EXIT_FAILURE;
    }
    int retCode = EXIT_FAILURE;
    omniClientReconnect(args[1].data());
    omniClientWait(omniClientCopy(args[1].data(), dstUrl, &retCode,
        [](void* userData, OmniClientResult result) noexcept
        {
            *(int*)userData = resultToRetcode(result);
            if (result != eOmniClientResult_Ok)
            {
                printResult(result);
                return;
            }
        }));
    return retCode;
}

int lock(ArgVec const& args)
{
    std::string url;
    if (args.size() <= 1)
    {
        if (g_stage)
        {
            url = g_stage->GetRootLayer()->GetRepositoryPath();
        }
        else
        {
            printf("No URL specified and no USD loaded\n");
            return EXIT_FAILURE;
        }
    }
    else
    {
        url = combineWithBaseUrl(args[1].data());
    }
    int retCode = EXIT_FAILURE;
    omniClientReconnect(url.c_str());
    omniClientWait(omniClientLock(url.c_str(), &retCode,
        [](void* userData, OmniClientResult result) noexcept
        {
            *(int*)userData = resultToRetcode(result);
            if (result != eOmniClientResult_Ok)
            {
                printResult(result);
                return;
            }
        }));
    return retCode;
}

int unlock(ArgVec const& args)
{
    std::string url;
    if (args.size() <= 1)
    {
        if (g_stage)
        {
            url = g_stage->GetRootLayer()->GetRepositoryPath();
        }
        else
        {
            printf("No URL specified and no USD loaded\n");
            return EXIT_FAILURE;
        }
    }
    else
    {
        url = combineWithBaseUrl(args[1].data());
    }
    int retCode = EXIT_FAILURE;
    omniClientReconnect(url.c_str());
    omniClientWait(omniClientUnlock(url.c_str(), &retCode,
        [](void* userData, OmniClientResult result) noexcept
        {
            *(int*)userData = resultToRetcode(result);
            if (result != eOmniClientResult_Ok)
            {
                printResult(result);
                return;
            }
        }));
    return retCode;
}

int disconnect(ArgVec const& args)
{
    if (args.size() <= 1)
    {
        auto combined = combineWithBaseUrl(".");
        omniClientSignOut(combined.c_str());
    }
    else
    {
        omniClientSignOut(args[1].data());
    }
    return EXIT_SUCCESS;
}

int joinChannel(ArgVec const& args)
{
    if (args.size() <= 1)
    {
        printf("Not enough arguments\n");
        return EXIT_FAILURE;
    }
    if (g_channel != 0)
    {
        omniClientStop(g_channel);
        g_channel = 0;
    }

    static int retCode = EXIT_FAILURE;
    omniClientReconnect(args[1].data());

    g_channel = omniClientJoinChannel(args[1].data(), nullptr,
        [](void* /* userData */, OmniClientResult result, OmniClientChannelEvent eventType, char const* from, struct OmniClientContent* content) noexcept
        {
            retCode = resultToRetcode(result);
            if (result == eOmniClientResult_Ok)
            {
                switch (eventType)
                {
                case eOmniClientChannelEvent_Error:
                    printf("Channel Unknown Error\n");
                    break;
                case eOmniClientChannelEvent_Message:
                    if (content && content->buffer)
                    {
                        char const* message = (char const*)content->buffer;
                        bool isAscii = (message[content->size - 1] == 0);
                        if (isAscii)
                        {
                            for (size_t i = 0; i < content->size - 1; i++)
                            {
                                if (message[i] < 32)
                                {
                                    isAscii = false;
                                    break;
                                }
                            }
                        }
                        if (isAscii)
                        {
                            printf("Channel Message from %s: %s\n", from, message);
                        }
                        else
                        {
                            printf("Channel Message from %s: <binary: %zd bytes>\n", from, content->size);
                        }
                    }
                    else
                    {
                        printf("Channel Message from %s: <null>\n", from);
                    }
                    break;
                case eOmniClientChannelEvent_Hello:
                    printf("Channel Hello from %s\n", from);
                    break;
                case eOmniClientChannelEvent_Join:
                    printf("Channel Join from %s\n", from);
                    break;
                case eOmniClientChannelEvent_Left:
                    printf("Channel Left from %s\n", from);
                    break;
                case eOmniClientChannelEvent_Deleted:
                    printf("Channel Deleted\n");
                    break;
                default:
                    break;
                }
            }
            else
            {
                printf("Channel Error: %s\n", omniClientGetResultString(result));
            }
        });

    omniClientWait(g_channel);
    return retCode;
}

int leaveChannel(ArgVec const&)
{
    if (g_channel != 0)
    {
        omniClientStop(g_channel);
        g_channel = 0;
    }
    return EXIT_SUCCESS;
}

int sendMessage(ArgVec const& args)
{
    if (g_channel == 0)
    {
        printf("Not in a channel\n");
        return EXIT_FAILURE;
    }
    char const* message = "";
    if (args.size() > 1)
    {
        message = args[1].data();
    }
    int retCode = EXIT_FAILURE;
    auto content = omniClientReferenceContent((void*)message, strlen(message) + 1);
    omniClientWait(omniClientSendMessage(g_channel, &content, &retCode,
        [](void* userData, OmniClientResult result) noexcept
        {
            *(int*)userData = resultToRetcode(result);
            if (result != eOmniClientResult_Ok)
            {
                printResult(result);
                return;
            }
        }));
    return retCode;
}

using CommandFn = int (*)(ArgVec const& args);
struct Command
{
    char const* name;
    char const* args;
    char const* helpMessage;
    CommandFn function;
};

// A new line (\n) in the message string results in the string that follows it
// to be printed on a second line
Command commands[] = {
    { "help", nullptr, "Print this help message", help },
    { "--help", nullptr, nullptr, help },
    { "-h", nullptr, nullptr, help },
    { "-?", nullptr, nullptr, help },
    { "/?", nullptr, nullptr, help },
    { "quit", nullptr, "Quit the interactive terminal", noop },
    { "log", "<level>", "Change the log level", logLevel },
    { "list", "<url>", "List the contents of a folder", list },
    { "ls", nullptr, nullptr, list },
    { "dir", nullptr, nullptr, list },
    { "stat", "<url>", "Print information about a specified file or folder", stat },
    { "cd", "<url>", "Makes paths relative to the specified folder", cd },
    { "push", "<url>", "Makes paths relative to the specified folder\n You can restore the original folder with pop", push },
    { "pushd", nullptr, nullptr, push },
    { "pop", nullptr, "Restores a folder pushed with 'push'", pop },
    { "popd", nullptr, nullptr, pop },
    { "copy", "<src> <dst>", "Copies a file or folder from src to dst (overwrites dst)", copy },
    { "cp", nullptr, nullptr, copy },
    { "move", "<src> <dst>", "Moves a file or folder from src to dst (overwrites dst)", move },
    { "mv", nullptr, nullptr, move },
    { "del", "<url>", "Deletes the specified file or folder", del },
    { "delete", nullptr, nullptr, del },
    { "rm", nullptr, nullptr, del },
    { "mkdir", "<url>", "Create a folder", mkdir },
    { "cat", "<url>", "Print the contents of a file", cat },
    { "cver", nullptr, "Print the client version", clientVersion },
    { "rver", nullptr, "Print the USD Resolver Plugin version", resolverVersion },
    { "sver", "<url>", "Print the server version", serverVersion },
    { "load", "<url>", "Load a USD file", loadUsd },
    { "save", "[url]", "Save a previously loaded USD file (optionally to a different URL)", saveUsd },
    { "close", nullptr, "Close a previously loaded USD file", closeUsd },
    { "lock", "[url]", "Lock a USD file (defaults to loaded stage root)", lock },
    { "unlock", "[url]", "Unlock a USD file (defaults to loaded stage root)", unlock },
    { "getacls", "<url>", "Print the ACLs for a URL", getacls },
    { "setacls", "<url> <user|group> <r|w|a|->", "Change the ACLs for a user or group for a URL\n Specify '-' to remove that user|group from the ACLs", setacls },
    { "auth", "[username] [password]", "Set username/password for authentication\n Password defaults to username; blank reverts to standard auth", auth },
    { "checkpoint", "<url> [comment]", "Create a checkpoint of a URL", makeCheckpoint },
    { "listCheckpoints", "<url>", "List all checkpoints of a URL", listCheckpoints },
    { "restoreCheckpoint", "<url>", "Restore a checkpoint", restoreCheckpoint },
    { "disconnect", "<url>", "Disconnect from a server", disconnect },
    { "join", "<url>", "Join a channel. Only one channel can be joined at a time.", joinChannel },
    { "send", "<message>", "Send a message to the joined channel.", sendMessage },
    { "leave", nullptr, "Leave the joined channel", leaveChannel },
};

int help(ArgVec const&)
{
    // Constants used in the formatting of the help message
    // Calculate the length of the longest "command <argument>" string
    size_t paddingCmdString = 0;
    for (auto&& command : commands)
    {
        paddingCmdString = std::max(paddingCmdString, std::string(command.name).length() + (command.args ? std::string(command.args).length() : 0) + 1);
    }
    std::string div(" : ");
    const auto paddingHelpString(paddingCmdString + div.size() + 1);

    for (auto&& command : commands)
    {
        if (command.helpMessage == nullptr)
        {
            continue;
        }

        // Formatting:
        // - 2 leading spaces,
        // - "command <arg>" string is left justified and padded to align all help strings,
        // - first "\n" in helpMessage breaks line
        std::string cmdNameAndArg(command.name);
        if (command.args != nullptr)
        {
            // Concatenate command and argument strings
            cmdNameAndArg += " ";
            cmdNameAndArg.append(command.args);
        }

        // Pad command_and_argument string so all ":" chars are aligned
        if (cmdNameAndArg.size() < paddingCmdString)
        {
            cmdNameAndArg += std::string(paddingCmdString - cmdNameAndArg.size(), ' ');
        }

        // Break help message at first "\n" character and print on 2 lines (subsequent "\n" are ignored)
        std::string helpMessage(command.helpMessage);
        auto pos = helpMessage.find("\n");
        if (pos != std::string::npos)
        {
            printf("  %s%s%s\n%s\n", cmdNameAndArg.c_str(), div.c_str(), helpMessage.substr(0, pos).c_str(),
                (std::string(paddingHelpString, ' ') + helpMessage.substr(pos + 1)).c_str());
        }
        else
        {
            printf("  %s%s%s\n", cmdNameAndArg.c_str(), div.c_str(), command.helpMessage);
        }
    }
    return EXIT_SUCCESS;
}

int run(ArgVec const& args)
{
    if (args.size() == 0)
    {
        return EXIT_FAILURE;
    }
    for (auto&& command : commands)
    {
        if (iequal(args[0], command.name))
        {
            return command.function(args);
        }
    }
    printf("Unknown command \"%s\":  Type \"help\" to list available commands.\n", args[0].data());
    return EXIT_FAILURE;
}

int main(int argc, char const* const* argv)
{
    ArgVec args;
    if (argc > 1)
    {
        args.resize(argc - 1);
        for (int i = 1; i < argc; i++)
        {
            args[i - 1] = argv[i];
        }
        return run(args);
    }
    omniClientSetLogCallback(
        [](char const* /* threadName */, char const* /* component */, OmniClientLogLevel level, char const* message) noexcept
        {
            bool isPing = strstr(message, "\"command\":\"ping\"") != nullptr;
            if (!isPing)
            {
                // Some versions of the server don't include "command:ping" in the reply
                // Try to detect these ping replies based on other fields being present or not
                bool hasVersion = strstr(message, "\"version\":") != nullptr;
                bool hasAuth = strstr(message, "\"auth\":") != nullptr;
                if (hasVersion && hasAuth)
                {
                    isPing = true;
                }
                bool hasToken = strstr(message, "\"token\":") != nullptr;
                bool hasServerCaps = strstr(message, "\"server_capabilities\":") != nullptr;
                if (hasToken && !hasServerCaps)
                {
                    isPing = true;
                }
            }
            if (!isPing)
            {
                printf("%c: %s\n", omniClientGetLogLevelChar(level), message);
            }
        });
    if (!omniClientInitialize(kOmniClientVersion))
    {
        printf("Failed to initialize Omniverse Client Library\n");
        return EXIT_FAILURE;
    }
    omniClientSetLogLevel(eOmniClientLogLevel_Warning);

    omniClientRegisterFileStatusCallback(nullptr,
        [](void* /* userData */, char const* url, OmniClientFileStatus status, int percentage) noexcept
        {
            printf("%s (%d%%): %s\n", omniClientGetFileStatusString(status), percentage, url);
        });

    auto updateThread = std::thread(
        []()
        {
            auto lock = make_lock(g_mutex);
            while (!g_shutdown)
            {
                if (g_haveUpdates)
                {
                    g_haveUpdates = false;
                    printf("Processing live updates\n");
                    omniClientLiveProcess();
                }
                while (!g_haveUpdates && !g_shutdown)
                {
                    g_cv.wait(lock);
                }
            }
        });
    omniClientLiveSetQueuedCallback(
        []() noexcept
        {
            {
                auto lock = make_lock(g_mutex);
                printf("Live update queued\n");
                g_haveUpdates = true;
            }
            g_cv.notify_all();
        });

    for (;;)
    {
        printf("> ");
        char line[5000];
        if (fgets(line, sizeof(line), stdin) == nullptr)
        {
            return EXIT_FAILURE;
        }
        auto tokens = tokenize(line);
        if (tokens.size() == 0)
        {
            continue;
        }
        if (iequal(tokens[0], "quit") || iequal(tokens[0], "exit") || iequal(tokens[0], "q"))
        {
            break;
        }
        args.resize(tokens.size());
        for (size_t i = 0; i < tokens.size(); i++)
        {
            args[i] = tokens[i];
        }
        run(args);
    }

    {
        auto lock = make_lock(g_mutex);
        g_shutdown = true;
    }
    g_cv.notify_all();
    updateThread.join();

    omniClientShutdown();

    return EXIT_SUCCESS;
}
