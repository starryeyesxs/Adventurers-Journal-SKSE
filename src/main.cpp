#include "PrismaUI_API.h"
#include "quest_text.h"
#include "quest_times.h"
#include <RE/B/BGSRefAlias.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef PlaySound
#	undef PlaySound
#endif

namespace
{
    PRISMA_UI_API::IVPrismaUI1* prismaUI = nullptr;
    PrismaView journalView = 0;
    bool journalOpen = false;
    bool journalViewReady = false;
    bool isTyping = false;
    bool journalPauseOwned = false;
    namespace fs = std::filesystem;
    using json = nlohmann::json;

    struct JournalSettings
    {
        bool pauseGameWhenOpen = false;
        bool collapseTasksByDefault = true;
        bool showChroniclesPluginName = true;
        json labels = {
            { "tracked", "Tracked" }, { "main", "Main" }, { "companions", "The Companions" },
            { "college", "College of Winterhold" }, { "thieves", "Thieves Guild" },
            { "brotherhood", "Dark Brotherhood" }, { "civilWar", "Civil War" },
            { "imperial", "Imperial Legion" }, { "stormcloaks", "Stormcloaks" },
            { "side", "Side & Daedric" }, { "chronicles", "Chronicles" },
            { "tasks", "Tasks & Errands" }
        };
    };
    JournalSettings settings;

    constexpr std::string_view kDefaultIni = "; Adventurer's Journal settings. In case you feel like adding a bit of your own flair to fit your playthrough " R"ini(
; Boolean values are 0 (off) or 1 (on).
[General]
; Change value to 1 here in case you're scared and need the game to pause while checking journal
bPauseGameWhenOpen=0
; Open journal with Misc quests section collapsed by default. Change to 0 if you don't care.
bCollapseTasksByDefault=1
; Cronicles includes CC and modded content. Shows plugin name of quest. On by default.
bShowChroniclesPluginName=1

; You can change the name of any of the quest sorting headers here.
[CategoryLabels]
sTracked=Tracked
sMain=Main
sCompanions=The Companions
sCollege=College of Winterhold
sThievesGuild=Thieves Guild
sDarkBrotherhood=Dark Brotherhood
sCivilWarFallback=Civil War
sImperialLegion=Imperial Legion
sStormcloaks=Stormcloaks
sSideAndDaedric=Side & Daedric
sChronicles=Chronicles
sTasksAndErrands=Tasks & Errands
)ini";

    struct CachedScreenshot
    {
        fs::file_time_type lastModified;
        std::string dataUrl;
    };

    std::mutex screenshotCacheMutex;
    std::unordered_map<std::string, CachedScreenshot> screenshotCache;
    std::atomic<std::uint64_t> saveGeneration{ 0 };
    std::atomic<bool> saveReady{ false };

    // Vanilla Skyrim sound descriptors — FormIDs verified against Skyrim.esm binary
    constexpr RE::FormID kSoundOpen     = 0x00077C00;  // UIJournalOpen
    constexpr RE::FormID kSoundClose    = 0x00077C01;  // UIJournalClose
    constexpr RE::FormID kSoundPageTurn = 0x0007EA3C;  // ITMBookPageTurnForwardSD
    constexpr RE::FormID kMiscellaneousQuestFormID = 0x00039F26;

    fs::path UserDataDirectory()
    {
        wchar_t buffer[32768]{};
        const auto length = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        if (length == 0 || length >= std::size(buffer)) {
            return {};
        }
        const fs::path root(buffer, buffer + length);
        return root.parent_path() / "Data" / "SKSE" / "Plugins" / "AdventurersJournal" / "UserData";
    }

    std::string_view TrimIni(std::string_view value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
            return {};
        }
        return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    }

    void LoadSettings()
    {
        const auto userDirectory = UserDataDirectory();
        if (userDirectory.empty()) {
            logger::error("AdventurersJournal: cannot determine INI directory");
            return;
        }
        const auto path = userDirectory.parent_path().parent_path() / "AdventurersJournal.ini";
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error) {
            logger::error("AdventurersJournal: cannot create INI directory: {}", error.message());
            return;
        }

        const auto file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            std::string defaultIni;
            defaultIni.reserve(kDefaultIni.size() + std::count(kDefaultIni.begin(), kDefaultIni.end(), '\n'));
            for (const char c : kDefaultIni) {
                if (c == '\n') {
                    defaultIni += '\r';
                }
                defaultIni += c;
            }
            DWORD written = 0;
            const auto length = static_cast<DWORD>(defaultIni.size());
            if (!::WriteFile(file, defaultIni.data(), length, &written, nullptr) || written != length) {
                logger::error("AdventurersJournal: failed to write default INI (error {})", ::GetLastError());
            }
            ::CloseHandle(file);
        } else if (::GetLastError() != ERROR_FILE_EXISTS) {
            logger::error("AdventurersJournal: cannot create INI (error {})", ::GetLastError());
        }

        std::ifstream input(path, std::ios::binary);
        if (!input) {
            logger::error("AdventurersJournal: cannot read INI '{}'", path.string());
            return;
        }
        static constexpr std::array labelKeys{
            std::pair{ "sTracked", "tracked" }, std::pair{ "sMain", "main" },
            std::pair{ "sCompanions", "companions" }, std::pair{ "sCollege", "college" },
            std::pair{ "sThievesGuild", "thieves" }, std::pair{ "sDarkBrotherhood", "brotherhood" },
            std::pair{ "sCivilWarFallback", "civilWar" }, std::pair{ "sImperialLegion", "imperial" },
            std::pair{ "sStormcloaks", "stormcloaks" }, std::pair{ "sSideAndDaedric", "side" },
            std::pair{ "sChronicles", "chronicles" }, std::pair{ "sTasksAndErrands", "tasks" }
        };
        std::string section;
        std::string line;
        bool firstLine = true;
        while (std::getline(input, line)) {
            std::string_view text(line);
            if (firstLine && text.starts_with("\xEF\xBB\xBF")) {
                text.remove_prefix(3);
            }
            firstLine = false;
            text = TrimIni(text);
            if (text.empty() || text.front() == ';' || text.front() == '#') {
                continue;
            }
            if (text.front() == '[' && text.back() == ']') {
                section = std::string(TrimIni(text.substr(1, text.size() - 2)));
                continue;
            }
            const auto separator = text.find('=');
            if (separator == std::string_view::npos) {
                logger::warn("AdventurersJournal: ignoring malformed INI line '{}'", text);
                continue;
            }
            const auto key = TrimIni(text.substr(0, separator));
            const auto value = TrimIni(text.substr(separator + 1));
            if (quest_text::EqualIgnoreCase(section, "General")) {
                bool* setting = nullptr;
                if (quest_text::EqualIgnoreCase(key, "bPauseGameWhenOpen")) setting = &settings.pauseGameWhenOpen;
                else if (quest_text::EqualIgnoreCase(key, "bCollapseTasksByDefault")) setting = &settings.collapseTasksByDefault;
                else if (quest_text::EqualIgnoreCase(key, "bShowChroniclesPluginName")) setting = &settings.showChroniclesPluginName;
                if (setting) {
                    if (value == "0" || value == "1") *setting = value == "1";
                    else logger::warn("AdventurersJournal: invalid boolean INI setting '{}={}'", key, value);
                }
            } else if (quest_text::EqualIgnoreCase(section, "CategoryLabels")) {
                for (const auto& [iniKey, jsonKey] : labelKeys) {
                    if (quest_text::EqualIgnoreCase(key, iniKey)) {
                        if (value.empty()) {
                            logger::warn("AdventurersJournal: empty category label '{}'", key);
                        } else if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0) == 0) {
                            logger::warn("AdventurersJournal: invalid UTF-8 category label '{}'", key);
                        } else {
                            settings.labels[jsonKey] = std::string(value);
                        }
                        break;
                    }
                }
            }
        }
        if (input.bad()) {
            logger::error("AdventurersJournal: error reading INI '{}'", path.string());
        }
    }

    std::string FileUrl(const fs::path& path)
    {
        const auto native = path.u8string();
        std::string url = "file:///";
        for (const auto character : native) {
            const auto value = static_cast<unsigned char>(character);
            if (std::isalnum(value) || character == '/' || character == ':' || character == '.' ||
                character == '-' || character == '_' || character == '~') {
                url += static_cast<char>(character);
            } else {
                url += std::format("%{:02X}", value);
            }
        }
        return url;
    }

    std::string ReadPngDataUrl(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return {};
        }

        const std::string bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        static constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        encoded.reserve((bytes.size() + 2) / 3 * 4);
        for (std::size_t index = 0; index < bytes.size(); index += 3) {
            const auto first = static_cast<unsigned char>(bytes[index]);
            const auto second = index + 1 < bytes.size() ? static_cast<unsigned char>(bytes[index + 1]) : 0;
            const auto third = index + 2 < bytes.size() ? static_cast<unsigned char>(bytes[index + 2]) : 0;
            encoded += alphabet[first >> 2];
            encoded += alphabet[((first & 0x03) << 4) | (second >> 4)];
            encoded += index + 1 < bytes.size() ? alphabet[((second & 0x0F) << 2) | (third >> 6)] : '=';
            encoded += index + 2 < bytes.size() ? alphabet[third & 0x3F] : '=';
        }
        return "data:image/png;base64," + encoded;
    }

    void AtomicWriteJson(const fs::path& path, const json& value)
    {
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error) {
            logger::error("AdventurersJournal: cannot create user-data directory '{}'", path.parent_path().string());
            return;
        }

        const auto temporary = path.wstring() + L".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                logger::error("AdventurersJournal: cannot write temporary user-data file '{}'", path.string());
                return;
            }
            output << value.dump(2);
        }
        fs::remove(path, error);
        fs::rename(temporary, path, error);
        if (error) {
            logger::error("AdventurersJournal: cannot replace user-data file '{}'", path.string());
            fs::remove(temporary, error);
        }
    }

    json ReadJson(const fs::path& path, json fallback)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return fallback;
        }
        try {
            return json::parse(input);
        } catch (const json::parse_error& error) {
            logger::warn("AdventurersJournal: invalid user-data JSON '{}': {}", path.string(), error.what());
            return fallback;
        }
    }

    json SnapshotFieldNotes();

    void SendJournalUserData()
    {
        if (!journalViewReady || !prismaUI || !prismaUI->IsValid(journalView)) {
            return;
        }

        const auto metadataPath = UserDataDirectory() / "sketch-metadata.json";
        const auto questCategoriesPath = UserDataDirectory() / "quest-categories.json";
        prismaUI->Invoke(journalView, ("window.loadNativeUserData(" + json{
            { "fieldNotes", SnapshotFieldNotes() },
            { "saveGeneration", saveGeneration.load() },
            { "sketchMetadata", ReadJson(metadataPath, json::object()) },
            { "sketchMetadataExists", fs::exists(metadataPath) },
            { "questCategories", ReadJson(questCategoriesPath, json::object()) },
            { "settings", {
                { "pauseGameWhenOpen", settings.pauseGameWhenOpen },
                { "collapseTasksByDefault", settings.collapseTasksByDefault },
                { "showChroniclesPluginName", settings.showChroniclesPluginName },
                { "labels", settings.labels }
            } }
        }.dump(-1, ' ', false, json::error_handler_t::replace) + ")").c_str());
    }

    void SendJournalScreenshots()
    {
        if (!journalViewReady || !prismaUI || !prismaUI->IsValid(journalView)) {
            return;
        }

        json screenshots = json::array();
        std::error_code error;
        const auto screenshotDirectory = UserDataDirectory().parent_path().parent_path().parent_path().parent_path().parent_path();
        if (fs::exists(screenshotDirectory, error)) {
            for (const auto& entry : fs::directory_iterator(screenshotDirectory, error)) {
                if (error) {
                    logger::warn("AdventurersJournal: failed to enumerate screenshots in '{}': {}",
                        screenshotDirectory.string(), error.message());
                    break;
                }
                if (!entry.is_regular_file(error)) {
                    if (error) {
                        logger::warn("AdventurersJournal: failed to inspect screenshot entry: {}", error.message());
                        error.clear();
                    }
                    continue;
                }
                auto extension = entry.path().extension().wstring();
                for (auto& character : extension) {
                    character = static_cast<wchar_t>(std::towlower(character));
                }
                if (extension != L".png") {
                    continue;
                }
                const auto modified = fs::last_write_time(entry.path(), error);
                if (error) {
                    logger::warn("AdventurersJournal: could not read timestamp for screenshot '{}': {}",
                        entry.path().string(), error.message());
                    error.clear();
                    continue;
                }
                const auto filename = entry.path().filename().string();
                std::string dataUrl;
                {
                    const std::lock_guard lock(screenshotCacheMutex);
                    auto cached = screenshotCache.find(filename);
                    if (cached == screenshotCache.end() || cached->second.lastModified != modified) {
                        dataUrl = ReadPngDataUrl(entry.path());
                        if (dataUrl.empty()) {
                            logger::warn("AdventurersJournal: could not read screenshot '{}'", entry.path().string());
                            screenshotCache.erase(filename);
                            continue;
                        }
                        screenshotCache.insert_or_assign(filename, CachedScreenshot{ modified, dataUrl });
                    } else {
                        dataUrl = cached->second.dataUrl;
                    }
                }
                screenshots.push_back({
                    { "id", entry.path().filename().u8string() },
                    { "src", dataUrl },
                    { "filename", entry.path().filename().u8string() }
                });
            }
        }
        logger::info("AdventurersJournal: scanned screenshot path '{}' and found {} PNG file(s)",
            screenshotDirectory.string(), screenshots.size());
        prismaUI->Invoke(journalView, ("window.loadScreenshotsFromSkyrim(" + screenshots.dump() + ")").c_str());
    }

    void PlayJournalSound(RE::FormID formID)
    {
        const auto taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            logger::warn("PlayJournalSound: task interface unavailable for sound 0x{:08X}", formID);
            return;
        }

        taskInterface->AddTask([formID] {
            const auto dataHandler = RE::TESDataHandler::GetSingleton();
            const auto descriptor = dataHandler
                ? dataHandler->LookupForm<RE::BGSSoundDescriptorForm>(formID, "Skyrim.esm")
                : nullptr;

            if (!descriptor) {
                logger::error("PlayJournalSound: BGSSoundDescriptorForm 0x{:08X} not found in Skyrim.esm", formID);
                return;
            }

            auto* audioManager = RE::BSAudioManager::GetSingleton();
            RE::BSSoundHandle handle;
            if (audioManager && audioManager->GetSoundHandle(handle, descriptor)) {
                if (const auto player = RE::PlayerCharacter::GetSingleton(); player && player->Get3D()) {
                    handle.SetObjectToFollow(player->Get3D());
                }
                if (handle.Play()) {
                    return;
                }
            }
            logger::warn("PlayJournalSound: failed to play sound 0x{:08X}", formID);
        });
    }

    bool CanOpenJournal()
    {
        const auto ui = RE::UI::GetSingleton();
        const auto controlMap = RE::ControlMap::GetSingleton();
        if (!ui || !controlMap || ui->GameIsPaused()) {
            return false;
        }

        static constexpr std::array blockingMenus{
            "Main Menu"sv,
            "Loading Menu"sv,
            "Journal Menu"sv,
            "InventoryMenu"sv,
            "MagicMenu"sv,
            "MapMenu"sv,
            "TweenMenu"sv,
            "Dialogue Menu"sv,
            "ContainerMenu"sv,
            "Crafting Menu"sv,
            "Lockpicking Menu"sv,
            "Console"sv,
            "MessageBoxMenu"sv
        };
        for (const auto menuName : blockingMenus) {
            if (ui->IsMenuOpen(menuName)) {
                return false;
            }
        }

        const auto& runtimeData = controlMap->GetRuntimeData();
        const auto& contexts = runtimeData.contextPriorityStack;
        return runtimeData.textEntryCount == 0 &&
               !contexts.empty() &&
               contexts.front() == RE::ControlMap::InputContextID::kGameplay;
    }

    bool IsControlMapTextInputActive()
    {
        const auto controlMap = RE::ControlMap::GetSingleton();
        return controlMap && controlMap->GetRuntimeData().textEntryCount > 0;
    }

    // Only touched on the game thread (via SKSE task queue) so the ControlMap
    // text-entry counter is incremented/decremented exactly once per transition.
    bool textInputHeld = false;

    void SetTextInputActive(bool active)
    {
        SKSE::GetTaskInterface()->AddTask([active]() {
            if (textInputHeld == active) {
                return;
            }
            if (const auto controlMap = RE::ControlMap::GetSingleton()) {
                controlMap->AllowTextInput(active);
                textInputHeld = active;
            }
        });
    }

    // Drops keyboard events from the list so other sinks (and their hotkeys) never see them.
    void StripKeyboardEvents(RE::InputEvent** head)
    {
        RE::InputEvent* first = nullptr;
        RE::InputEvent* last = nullptr;
        for (auto event = *head; event;) {
            const auto next = event->next;
            const auto isKeyboard = event->GetDevice() == RE::INPUT_DEVICE::kKeyboard;
            if (!isKeyboard) {
                if (last) {
                    last->next = event;
                } else {
                    first = event;
                }
                last = event;
            }
            event = next;
        }
        if (last) {
            last->next = nullptr;
        }
        *head = first;
    }

    bool IsJournalUserEvent(const RE::InputEvent* event)
    {
        const auto userEvents = RE::UserEvents::GetSingleton();
        return userEvents && event && event->QUserEvent() == userEvents->journal;
    }

    void CloseVanillaJournal()
    {
        if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
            queue->AddMessage(RE::BSFixedString(RE::JournalMenu::MENU_NAME), RE::UI_MESSAGE_TYPE::kForceHide, nullptr);
        }
    }

    std::string EscapeJson(std::string_view value)
    {
        std::string result;
        result.reserve(value.size() + 2);
        for (const auto character : value) {
            switch (character) {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(character) < 0x20) {
                    result += std::format("\\u{:04x}", static_cast<unsigned int>(static_cast<unsigned char>(character)));
                } else {
                    result += character;
                }
                break;
            }
        }
        return result;
    }

    void AppendJsonString(std::string& output, std::string_view value)
    {
        output += '"';
        output += EscapeJson(value);
        output += '"';
    }

    std::string ResolveQuestText(RE::TESQuest* quest, std::string_view text)
    {
        return quest_text::Resolve(text, [quest](std::string_view type, std::string_view name)
            -> std::optional<std::string> {
            if (quest_text::EqualIgnoreCase(type, "Global")) {
                std::string editorID(name);
                // Some translations corrupt the J in MGRJ1 or terminate the tag early.
                if (quest_text::EqualIgnoreCase(std::string_view(editorID).substr(0, 5), "MGR)1")) {
                    editorID.replace(3, 1, "J");
                } else if (quest_text::EqualIgnoreCase(editorID, "MGR1Test")) {
                    editorID = "MGRJ1Test";
                }
                auto* global = RE::TESForm::LookupByEditorID<RE::TESGlobal>(editorID);
                if (!global) {
                    if (const auto data = RE::TESDataHandler::GetSingleton()) {
                        for (auto* candidate : data->GetFormArray<RE::TESGlobal>()) {
                            if (candidate && candidate->GetFormEditorID() &&
                                quest_text::EqualIgnoreCase(candidate->GetFormEditorID(), editorID)) {
                                global = candidate;
                                break;
                            }
                        }
                    }
                }
                if (global && std::isfinite(global->value)) {
                    return std::format("{:g}", global->value);
                }
                const auto questID = quest ? quest->GetFormEditorID() : nullptr;
                if (questID && (quest_text::EqualIgnoreCase(questID, "MGRAppJzargo01") ||
                                quest_text::EqualIgnoreCase(questID, "MGRitual01")) &&
                    (quest_text::EqualIgnoreCase(editorID, "MGR1") ||
                     quest_text::EqualIgnoreCase(editorID, "MGRJ1"))) {
                    return "3";
                }
            } else if (quest && (quest_text::EqualIgnoreCase(type, "Alias") ||
                                 quest_text::EqualIgnoreCase(type, "Alias.CurrentName"))) {
                for (const auto alias : quest->aliases) {
                    if (!alias || !alias->aliasName.c_str() ||
                        !quest_text::EqualIgnoreCase(alias->aliasName.c_str(), name)) {
                        continue;
                    }

                    RE::TESObjectREFR* reference = nullptr;
                    const auto resolvedReference = quest->GetAliasedRef(alias->aliasID).get();
                    reference = resolvedReference.get();

                    if (!reference) {
                        const auto refAlias = dynamic_cast<RE::BGSRefAlias*>(alias);
                        reference = refAlias ? refAlias->GetReference() : nullptr;
                    }

                    if (reference) {
                        if (const auto displayName = reference->GetName(); displayName && *displayName != '\0') {
                            return std::string(displayName);
                        } else if (const auto form = reference->GetObjectReference();
                                   form && form->GetName() && *form->GetName() != '\0') {
                            return std::string(form->GetName());
                        }
                    }

                    for (const auto instance : quest->instanceData) {
                        if (!instance || instance->id != quest->currentInstanceID) {
                            continue;
                        }
                        for (const auto& savedName : instance->stringData) {
                            if (savedName.aliasID != alias->aliasID || savedName.fullNameFormID == 0) {
                                continue;
                            }
                            const auto savedForm = RE::TESForm::LookupByID(savedName.fullNameFormID);
                            if (savedForm) {
                                if (const auto savedDisplayName = savedForm->GetName();
                                    savedDisplayName && *savedDisplayName != '\0') {
                                    return std::string(savedDisplayName);
                                }
                            }
                        }
                    }
                    break;
                }
            }
            logger::warn("Quest text replacement unresolved: quest='{}', type='{}', name='{}'",
                quest && quest->GetFullName() ? quest->GetFullName() : "<unnamed>", type, name);
            return std::nullopt;
        });
    }

    const char* QuestName(const RE::TESQuest* quest)
    {
        if (const auto name = quest->GetFullName(); name && *name != '\0') {
            return name;
        }
        return "";
    }

    void AppendQuestLog(std::string& output, RE::TESQuest* quest, RE::PlayerCharacter* player)
    {
        bool firstEntry = true;
        output += '"';
        RE::BSString journalText;
        quest->GetJournalTextForInstance(journalText, quest->currentInstanceID);
        if (!journalText.empty()) {
            output += EscapeJson(ResolveQuestText(quest, journalText.c_str()));
            firstEntry = false;
        }

        if (!journalText.empty()) {
            output += '"';
            return;
        }

        for (const auto stageItem : player->GetQuestLog()) {
            if (!stageItem || stageItem->owner != quest || !stageItem->hasLogEntry) {
                continue;
            }

            const auto logEntry = stageItem->GetLogEntry(quest);
            if (!logEntry || *logEntry == '\0') {
                continue;
            }

            if (!firstEntry) {
                output += "\\n\\n";
            }
            output += EscapeJson(ResolveQuestText(quest, logEntry));
            firstEntry = false;
        }
        output += '"';
    }

    // Tamrielic accepted/completed stamps persisted in the SKSE cosave.
    std::mutex questTimesMutex;
    std::unordered_map<RE::FormID, QuestTimes> questTimes;
    std::unordered_set<RE::FormID> observedActiveQuests;
    std::unordered_set<RE::FormID> abandonedQuests;
    std::uint64_t nextCompletionOrder = 0;

    constexpr std::uint32_t MakeTag(char a, char b, char c, char d)
    {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
               static_cast<std::uint32_t>(static_cast<unsigned char>(d));
    }

    constexpr std::uint32_t kSerializationUID = MakeTag('A', 'J', 'N', 'L');
    constexpr std::uint32_t kQuestTimesRecord = MakeTag('Q', 'T', 'M', 'S');
    constexpr std::uint32_t kQuestTimesVersion = 2;
    constexpr std::uint32_t kAbandonedQuestsRecord = MakeTag('A', 'B', 'N', 'D');
    constexpr std::uint32_t kAbandonedQuestsVersion = 1;
    constexpr std::uint32_t kMaxStampLength = 256;

    bool GetNowStamp(float& days, std::string& date)
    {
        const auto calendar = RE::Calendar::GetSingleton();
        if (!calendar) {
            return false;
        }
        char buffer[128]{};
        calendar->GetTimeDateString(buffer, sizeof(buffer), true);
        days = calendar->GetDaysPassed();
        date = buffer;
        return !date.empty();
    }

    std::optional<QuestTimes> GetQuestTimes(RE::FormID formID)
    {
        std::scoped_lock lock(questTimesMutex);
        const auto it = questTimes.find(formID);
        return it != questTimes.end() ? std::optional<QuestTimes>(it->second) : std::nullopt;
    }

    void AppendQuestTimes(std::string& output, const std::optional<QuestTimes>& times)
    {
        if (!times) {
            return;
        }
        if (times->hasAccepted) {
            output += ",\"acceptedDate\":";
            AppendJsonString(output, times->acceptedDate);
        }
        if (times->hasCompleted) {
            output += ",\"completedDate\":";
            AppendJsonString(output, times->completedDate);
        }
    }

    void AppendQuest(
        std::string& output,
        RE::TESQuest* quest,
        RE::PlayerCharacter* player,
        const std::optional<QuestTimes>& times,
        std::size_t order)
    {
        output += "{\"id\":";
        AppendJsonString(output, std::format("{:08X}", quest->GetFormID()));
        output += ",\"order\":";
        output += std::to_string(order);
        output += ",\"tracked\":";
        output += quest->IsActive() ? "true" : "false";
        output += ",\"questType\":";
        output += std::to_string(static_cast<unsigned>(quest->GetType()));
        output += ",\"sourcePlugin\":";
        if (const auto file = quest->GetFile(0)) {
            AppendJsonString(output, file->GetFilename());
        } else {
            output += "null";
            logger::warn("AdventurersJournal: quest 0x{:08X} has no source plugin", quest->GetFormID());
        }
        output += ",\"title\":";
        AppendJsonString(output, QuestName(quest));
        AppendQuestTimes(output, times);
        output += ",\"body\":";
        AppendQuestLog(output, quest, player);
        output += ",\"objectives\":[";

        bool firstObjective = true;
        for (const auto objective : quest->objectives) {
            if (!objective || !objective->initialized) {
                continue;
            }

            const auto state = objective->state.get();
            const bool completed = state == RE::QUEST_OBJECTIVE_STATE::kCompleted ||
                                   state == RE::QUEST_OBJECTIVE_STATE::kCompletedDisplayed;
            if (state == RE::QUEST_OBJECTIVE_STATE::kDormant || state == RE::QUEST_OBJECTIVE_STATE::kFailed ||
                state == RE::QUEST_OBJECTIVE_STATE::kFailedDisplayed) {
                continue;
            }

            if (!firstObjective) {
                output += ",";
            }
            output += "{\"text\":";
            AppendJsonString(output, ResolveQuestText(quest, objective->displayText.c_str()));
            output += ",\"done\":";
            output += completed ? "true" : "false";
            output += "}";
            firstObjective = false;
        }

        output += "]}";
    }

    bool HasVisibleJournalLog(RE::PlayerCharacter* player, RE::TESQuest* quest)
    {
        if (!player) {
            return false;
        }

        for (const auto stageItem : player->GetQuestLog()) {
            if (!stageItem || stageItem->owner != quest || !stageItem->hasLogEntry) {
                continue;
            }

            const auto logEntry = stageItem->GetLogEntry(quest);
            if (logEntry && *logEntry != '\0') {
                return true;
            }
        }
        return false;
    }

    bool HasVisibleObjectiveForActive(RE::TESQuest* quest)
    {
        for (const auto objective : quest->objectives) {
            if (!objective || !objective->initialized) {
                continue;
            }
            const auto state = objective->state.get();
            const bool displayed = state == RE::QUEST_OBJECTIVE_STATE::kDisplayed;
            const bool completed = state == RE::QUEST_OBJECTIVE_STATE::kCompleted ||
                                   state == RE::QUEST_OBJECTIVE_STATE::kCompletedDisplayed;
            if (displayed && !completed && !objective->displayText.empty()) {
                return true;
            }
        }
        return false;
    }

    bool HasVisibleObjectiveForCompleted(RE::TESQuest* quest)
    {
        for (const auto objective : quest->objectives) {
            if (!objective || !objective->initialized) {
                continue;
            }
            const auto state = objective->state.get();
            const bool visibleState = state == RE::QUEST_OBJECTIVE_STATE::kDisplayed ||
                                      state == RE::QUEST_OBJECTIVE_STATE::kCompleted ||
                                      state == RE::QUEST_OBJECTIVE_STATE::kCompletedDisplayed;
            if (visibleState && !objective->displayText.empty()) {
                return true;
            }
        }
        return false;
    }

    // Match JournalMenu's player-facing boundary: an engine quest is not
    // journal-visible merely because it exists, runs, or has dormant data.
    // It needs a visible objective or a non-empty journal stage log.
    bool ShouldShowQuest(RE::TESQuest* quest, RE::PlayerCharacter* player)
    {
        if (!quest) {
            return false;
        }

        const auto fullName = quest->GetFullName();
        if (!fullName || *fullName == '\0') {
            return false;
        }

        const bool completed = quest->IsCompleted();
        if (completed) {
            return HasVisibleJournalLog(player, quest) || HasVisibleObjectiveForCompleted(quest);
        }

        return quest->IsRunning() && HasVisibleObjectiveForActive(quest);
    }

    // Stamps first-seen (accepted) and completion times. `visible` skips the journal-visibility
    // check when the caller has already performed it.
    bool ReconcileQuestTimes(RE::TESQuest* quest, RE::PlayerCharacter* player, bool visible = false,
                             bool completionEvent = false)
    {
        if (!quest || !saveReady.load()) {
            return false;
        }

        const auto generation = saveGeneration.load();
        const auto formID = quest->GetFormID();
        const bool completed = quest->IsCompleted();

        if (completed) {
            if (!completionEvent) {
                return false;
            }
            std::scoped_lock lock(questTimesMutex);
            const auto it = questTimes.find(formID);
            if (it == questTimes.end() || !it->second.hasAccepted || it->second.hasCompleted ||
                !observedActiveQuests.contains(formID)) {
                return false;
            }
        }

        if (!visible && !ShouldShowQuest(quest, player)) {
            return false;
        }

        if (!completed) {
            std::scoped_lock lock(questTimesMutex);
            if (!saveReady.load() || generation != saveGeneration.load()) {
                return false;
            }
            observedActiveQuests.insert(formID);
        }

        float days = 0.0f;
        std::string date;
        if (!GetNowStamp(days, date)) {
            return false;
        }

        std::scoped_lock lock(questTimesMutex);
        if (!saveReady.load() || generation != saveGeneration.load()) {
            return false;
        }
        auto it = questTimes.find(formID);
        if (completed) {
            if (it == questTimes.end() || !it->second.hasAccepted || it->second.hasCompleted ||
                !observedActiveQuests.contains(formID)) {
                return false;
            }
            observedActiveQuests.erase(formID);
            it->second.hasCompleted = true;
            it->second.completedDays = days;
            it->second.completedDate = std::move(date);
            it->second.completionOrder = ++nextCompletionOrder;
            return true;
        }

        if (it == questTimes.end() || !it->second.hasAccepted) {
            auto& times = questTimes[formID];
            times.hasAccepted = true;
            times.acceptedDays = days;
            times.acceptedDate = std::move(date);
            return true;
        }

        // A repeatable quest running again is no longer completed.
        if (it->second.hasCompleted) {
            it->second.hasCompleted = false;
            it->second.completedDate.clear();
            it->second.completedDays = 0.0f;
            it->second.completionOrder = 0;
            it->second.acceptedDays = days;
            it->second.acceptedDate = std::move(date);
            return true;
        }
        return false;
    }

    void InitializeQuestTimes()
    {
        if (!saveReady.load()) {
            return;
        }
        const auto player = RE::PlayerCharacter::GetSingleton();
        const auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!player || !dataHandler) {
            logger::error("AdventurersJournal: cannot initialize quest timestamps without player and data handler");
            return;
        }
        float days = 0.0f;
        std::string date;
        if (!GetNowStamp(days, date)) {
            logger::error("AdventurersJournal: cannot initialize quest timestamps without an in-game date");
            return;
        }
        const auto generation = saveGeneration.load();
        for (const auto quest : dataHandler->GetFormArray<RE::TESQuest>()) {
            if (ShouldShowQuest(quest, player)) {
                {
                    std::scoped_lock lock(questTimesMutex);
                    if (!saveReady.load() || generation != saveGeneration.load()) {
                        return;
                    }
                    BackfillQuestTime(questTimes[quest->GetFormID()], quest->IsCompleted(), days, date);
                }
                ReconcileQuestTimes(quest, player);
            }
        }
    }

    // Field notes live only in the cosave so they stay strictly per-save. questTimesMutex
    // guards all cosave-backed state (quest times and notes).
    std::vector<std::string> fieldNotes;

    constexpr std::uint32_t kFieldNotesRecord = MakeTag('F', 'N', 'T', 'S');
    constexpr std::uint32_t kFieldNotesVersion = 1;
    constexpr std::uint32_t kMaxNoteLength = 1 << 16;
    constexpr std::uint32_t kMaxNoteCount = 1024;

    bool WriteString(SKSE::SerializationInterface* intfc, const std::string& value, std::uint32_t maxLength)
    {
        const auto length = static_cast<std::uint32_t>(std::min<std::size_t>(value.size(), maxLength));
        return intfc->WriteRecordData(length) &&
               (length == 0 || intfc->WriteRecordData(value.data(), length));
    }

    bool ReadString(SKSE::SerializationInterface* intfc, std::string& value, std::uint32_t maxLength)
    {
        std::uint32_t length = 0;
        if (intfc->ReadRecordData(length) != sizeof(length) || length > maxLength) {
            return false;
        }
        value.assign(length, '\0');
        return length == 0 || intfc->ReadRecordData(value.data(), length) == length;
    }

    json SnapshotFieldNotes()
    {
        std::scoped_lock lock(questTimesMutex);
        return json(fieldNotes);
    }

    void SaveCosave(SKSE::SerializationInterface* intfc)
    {
        const auto generation = saveGeneration.load();
        if (!saveReady.load()) {
            logger::error("AdventurersJournal: refusing to save quest metadata during a load");
            return;
        }
        std::scoped_lock lock(questTimesMutex);
        if (!saveReady.load() || generation != saveGeneration.load()) {
            logger::error("AdventurersJournal: save transition interrupted quest metadata snapshot");
            return;
        }

        if (const auto data = RE::TESDataHandler::GetSingleton()) {
            for (auto* quest : data->GetFormArray<RE::TESQuest>()) {
                if (quest && quest->IsRunning() && !quest->IsCompleted()) {
                    auto& times = questTimes[quest->GetFormID()];
                    times.hasTracked = true;
                    times.tracked = quest->IsActive();
                }
            }
        } else {
            logger::error("AdventurersJournal: cannot snapshot tracked quests without the data handler");
        }

        if (intfc->OpenRecord(kQuestTimesRecord, kQuestTimesVersion)) {
            bool success = intfc->WriteRecordData(static_cast<std::uint32_t>(questTimes.size()));
            for (const auto& [formID, times] : questTimes) {
                const std::uint32_t id = formID;
                const std::uint8_t flags = (times.hasAccepted ? 1 : 0) | (times.hasCompleted ? 2 : 0) |
                                           (times.hasTracked ? 4 : 0) | (times.tracked ? 8 : 0);
                success = success && intfc->WriteRecordData(id) && intfc->WriteRecordData(flags) &&
                          intfc->WriteRecordData(times.acceptedDays) && intfc->WriteRecordData(times.completedDays) &&
                          WriteString(intfc, times.acceptedDate, kMaxStampLength) &&
                          WriteString(intfc, times.completedDate, kMaxStampLength) &&
                          intfc->WriteRecordData(times.completionOrder);
            }
            if (!success) {
                logger::error("AdventurersJournal: failed to write quest-times record");
            }
        } else {
            logger::error("AdventurersJournal: failed to open quest-times record");
        }

        if (intfc->OpenRecord(kFieldNotesRecord, kFieldNotesVersion)) {
            bool success = intfc->WriteRecordData(static_cast<std::uint32_t>(fieldNotes.size()));
            for (const auto& note : fieldNotes) {
                success = success && WriteString(intfc, note, kMaxNoteLength);
            }
            if (!success) {
                logger::error("AdventurersJournal: failed to write field-notes record");
            }
        } else {
            logger::error("AdventurersJournal: failed to open field-notes record");
        }

        if (intfc->OpenRecord(kAbandonedQuestsRecord, kAbandonedQuestsVersion)) {
            bool success = intfc->WriteRecordData(static_cast<std::uint32_t>(abandonedQuests.size()));
            for (const auto formID : abandonedQuests) {
                success = success && intfc->WriteRecordData(formID);
            }
            if (!success) {
                logger::error("AdventurersJournal: failed to write abandoned-quests record");
            }
        } else {
            logger::error("AdventurersJournal: failed to open abandoned-quests record");
        }
    }

    void LoadCosave(SKSE::SerializationInterface* intfc)
    {
        std::unordered_map<RE::FormID, QuestTimes> loadedTimes;
        std::vector<std::string> loadedNotes;
        std::unordered_set<RE::FormID> loadedAbandonedQuests;
        std::uint64_t loadedCompletionOrder = 0;

        std::uint32_t type = 0;
        std::uint32_t version = 0;
        std::uint32_t length = 0;
        while (intfc->GetNextRecordInfo(type, version, length)) {
            std::uint32_t count = 0;
            if (type == kQuestTimesRecord && (version == 1 || version == kQuestTimesVersion)) {
                if (intfc->ReadRecordData(count) != sizeof(count) || count > length / 17) {
                    logger::warn("AdventurersJournal: invalid quest-times record count");
                    continue;
                }
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::uint32_t oldID = 0;
                    std::uint8_t flags = 0;
                    QuestTimes times;
                    if (intfc->ReadRecordData(oldID) != sizeof(oldID) ||
                        intfc->ReadRecordData(flags) != sizeof(flags) ||
                        intfc->ReadRecordData(times.acceptedDays) != sizeof(float) ||
                        intfc->ReadRecordData(times.completedDays) != sizeof(float) ||
                        !ReadString(intfc, times.acceptedDate, kMaxStampLength) ||
                        !ReadString(intfc, times.completedDate, kMaxStampLength) ||
                        (version >= 2 && intfc->ReadRecordData(times.completionOrder) != sizeof(times.completionOrder))) {
                        logger::warn("AdventurersJournal: truncated quest-times record");
                        break;
                    }
                    times.hasAccepted = (flags & 1) != 0;
                    times.hasCompleted = (flags & 2) != 0;
                    times.hasTracked = (flags & 4) != 0;
                    times.tracked = (flags & 8) != 0;

                    RE::FormID newID = 0;
                    if (intfc->ResolveFormID(oldID, newID)) {
                        loadedCompletionOrder = (std::max)(loadedCompletionOrder, times.completionOrder);
                        loadedTimes[newID] = std::move(times);
                    } else {
                        logger::warn("AdventurersJournal: cannot resolve saved quest FormID 0x{:08X}", oldID);
                    }
                }
            } else if (type == kFieldNotesRecord && version == kFieldNotesVersion) {
                if (intfc->ReadRecordData(count) != sizeof(count) || count > kMaxNoteCount) {
                    logger::warn("AdventurersJournal: invalid field-notes record count");
                    continue;
                }
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::string note;
                    if (!ReadString(intfc, note, kMaxNoteLength)) {
                        logger::warn("AdventurersJournal: truncated field-notes record");
                        break;
                    }
                    loadedNotes.push_back(std::move(note));
                }
            } else if (type == kAbandonedQuestsRecord && version == kAbandonedQuestsVersion) {
                if (intfc->ReadRecordData(count) != sizeof(count) ||
                    length < sizeof(count) || count > (length - sizeof(count)) / sizeof(RE::FormID)) {
                    logger::warn("AdventurersJournal: invalid abandoned-quests record count");
                    continue;
                }
                for (std::uint32_t i = 0; i < count; ++i) {
                    RE::FormID oldID = 0;
                    if (intfc->ReadRecordData(oldID) != sizeof(oldID)) {
                        logger::warn("AdventurersJournal: truncated abandoned-quests record");
                        break;
                    }
                    RE::FormID newID = 0;
                    if (intfc->ResolveFormID(oldID, newID)) {
                        loadedAbandonedQuests.insert(newID);
                    } else {
                        logger::warn("AdventurersJournal: cannot resolve abandoned quest FormID 0x{:08X}", oldID);
                    }
                }
            } else if (type == kQuestTimesRecord || type == kFieldNotesRecord || type == kAbandonedQuestsRecord) {
                logger::warn("AdventurersJournal: unsupported cosave record version {} for type 0x{:08X}",
                    version, type);
            }
        }

        std::scoped_lock lock(questTimesMutex);
        questTimes = std::move(loadedTimes);
        fieldNotes = std::move(loadedNotes);
        abandonedQuests = std::move(loadedAbandonedQuests);
        nextCompletionOrder = loadedCompletionOrder;
    }

    void ClearSaveState()
    {
        std::scoped_lock lock(questTimesMutex);
        questTimes.clear();
        observedActiveQuests.clear();
        abandonedQuests.clear();
        fieldNotes.clear();
        nextCompletionOrder = 0;
    }

    void SetQuestTracked(RE::TESQuest* quest, bool tracked, bool forceRepath = true)
    {
        if (!quest) {
            return;
        }

        const bool activeChanged = quest->IsActive() != tracked;
        if (activeChanged) {
            quest->data.flags.set(tracked, RE::QuestFlag::kActive);
            quest->AddChange(RE::TESQuest::ChangeFlags::kQuestFlags);
        }

        bool parentChanged = false;
        if (tracked && quest->GetType() == RE::QUEST_DATA::Type::kMiscellaneous) {
            const auto miscellaneousQuest = RE::TESForm::LookupByID<RE::TESQuest>(kMiscellaneousQuestFormID);
            if (miscellaneousQuest && !miscellaneousQuest->IsActive()) {
                miscellaneousQuest->data.flags.set(true, RE::QuestFlag::kActive);
                miscellaneousQuest->AddChange(RE::TESQuest::ChangeFlags::kQuestFlags);
                parentChanged = true;
            } else if (!miscellaneousQuest) {
                logger::error("AdventurersJournal: vanilla Miscellaneous parent quest 0x{:08X} was not found",
                    kMiscellaneousQuestFormID);
            }
        }

        if (forceRepath && (activeChanged || parentChanged)) {
            if (const auto player = RE::PlayerCharacter::GetSingleton()) {
                player->GetPlayerFlags().forceQuestTargetRepath = true;
            }
        }
    }

    void RevertCosave(SKSE::SerializationInterface*)
    {
        ClearSaveState();
    }

    void RestoreQuestTracking()
    {
        const auto data = RE::TESDataHandler::GetSingleton();
        if (!data) {
            logger::error("AdventurersJournal: cannot restore tracked quests without the data handler");
            return;
        }
        std::scoped_lock lock(questTimesMutex);
        for (auto* quest : data->GetFormArray<RE::TESQuest>()) {
            if (!quest || !quest->IsRunning() || quest->IsCompleted()) {
                continue;
            }
            const auto it = questTimes.find(quest->GetFormID());
            if (it != questTimes.end() && it->second.hasTracked) {
                SetQuestTracked(quest, it->second.tracked, false);
            }
        }
        if (const auto player = RE::PlayerCharacter::GetSingleton()) {
            player->GetPlayerFlags().forceQuestTargetRepath = true;
        }
    }

    std::string BuildJournalJson()
    {
        if (!saveReady.load()) {
            return "{\"active\":[],\"completed\":[]}";
        }
        const auto player = RE::PlayerCharacter::GetSingleton();
        const auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!player || !dataHandler) {
            return "{\"active\":[],\"completed\":[]}";
        }

        std::string characterName = player->GetName() ? player->GetName() : "Unknown Adventurer";
        std::string currentDate = "Date unknown";
        if (const auto calendar = RE::Calendar::GetSingleton()) {
            char dateBuffer[128]{};
            calendar->GetTimeDateString(dateBuffer, sizeof(dateBuffer), true);
            if (dateBuffer[0] != '\0') {
                currentDate = dateBuffer;
            }
        }

        std::string output = "{\"characterName\":";
        AppendJsonString(output, characterName);
        output += ",\"date\":";
        AppendJsonString(output, currentDate);
        const auto imperial = RE::TESForm::LookupByID<RE::TESFaction>(0x0002BF9A);
        const auto stormcloaks = RE::TESForm::LookupByID<RE::TESFaction>(0x0002BF9B);
        if (!imperial || !stormcloaks) {
            logger::warn("AdventurersJournal: could not resolve Civil War factions");
        }
        output += ",\"civilWarAllegiance\":";
        AppendJsonString(output, imperial && player->IsInFaction(imperial) ? "imperial" :
            stormcloaks && player->IsInFaction(stormcloaks) ? "stormcloaks" : "none");

        std::unordered_set<RE::FormID> hiddenQuests;
        {
            std::scoped_lock lock(questTimesMutex);
            hiddenQuests = abandonedQuests;
        }

        // kCompleted takes strict precedence. A completed quest with kRunning still set
        // (common for post-quest dialogue) goes only to completed, never to active.
        std::vector<RE::TESQuest*> activeQuests;
        std::vector<RE::TESQuest*> completedQuests;
        for (const auto quest : dataHandler->GetFormArray<RE::TESQuest>()) {
            if (!ShouldShowQuest(quest, player)) {
                continue;
            }
            ReconcileQuestTimes(quest, player, true);
            if (hiddenQuests.contains(quest->GetFormID())) {
                continue;
            }
            if (quest->IsCompleted()) {
                completedQuests.push_back(quest);
            } else if (quest->IsRunning()) {
                activeQuests.push_back(quest);
            }
        }

        // Base order is kept in "order" so the UI can re-pin instantly when tracking changes.
        std::unordered_map<RE::FormID, std::size_t> activeOrder;
        for (std::size_t i = 0; i < activeQuests.size(); ++i) {
            activeOrder[activeQuests[i]->GetFormID()] = i;
        }
        std::stable_partition(activeQuests.begin(), activeQuests.end(), [](RE::TESQuest* quest) {
            return quest->IsActive();
        });

        // Recorded completion sequence takes precedence over older version-1 records.
        // Unsequenced quests fall back to their saved dates, then their live order.
        std::unordered_map<RE::FormID, std::optional<QuestTimes>> completedTimes;
        for (const auto quest : completedQuests) {
            completedTimes[quest->GetFormID()] = GetQuestTimes(quest->GetFormID());
        }
        const auto sortKey = [&completedTimes](RE::TESQuest* quest) -> std::optional<float> {
            const auto& times = completedTimes[quest->GetFormID()];
            if (!times) {
                return std::nullopt;
            }
            if (times->hasCompleted) {
                return times->completedDays;
            }
            if (times->hasAccepted) {
                return times->acceptedDays;
            }
            return std::nullopt;
        };
        std::stable_sort(completedQuests.begin(), completedQuests.end(), [&sortKey, &completedTimes](RE::TESQuest* a, RE::TESQuest* b) {
            const auto& timesA = completedTimes[a->GetFormID()];
            const auto& timesB = completedTimes[b->GetFormID()];
            const auto orderA = timesA && timesA->hasCompleted ? timesA->completionOrder : 0;
            const auto orderB = timesB && timesB->hasCompleted ? timesB->completionOrder : 0;
            if (orderA != orderB) {
                return orderA > orderB;
            }
            const auto keyA = sortKey(a);
            const auto keyB = sortKey(b);
            if (keyA.has_value() != keyB.has_value()) {
                return keyA.has_value();
            }
            return keyA && keyB && *keyA > *keyB;
        });

        output += ",\"active\":[";
        bool firstActive = true;
        for (const auto quest : activeQuests) {
            if (!firstActive) {
                output += ",";
            }
            AppendQuest(output, quest, player, GetQuestTimes(quest->GetFormID()), activeOrder[quest->GetFormID()]);
            firstActive = false;
        }

        output += "],\"completed\":[";
        std::size_t completedIndex = 0;
        for (const auto quest : completedQuests) {
            if (completedIndex > 0) {
                output += ",";
            }
            AppendQuest(output, quest, player, completedTimes[quest->GetFormID()], completedIndex);
            ++completedIndex;
        }
        output += "],\"abandonedQuests\":[";
        std::vector<RE::TESQuest*> hiddenQuestList;
        for (const auto formID : hiddenQuests) {
            if (auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(formID)) {
                hiddenQuestList.push_back(quest);
            } else {
                logger::warn("AdventurersJournal: abandoned quest 0x{:08X} not found", formID);
            }
        }
        std::sort(hiddenQuestList.begin(), hiddenQuestList.end(), [](const auto* a, const auto* b) {
            const auto comparison = std::strcmp(QuestName(a), QuestName(b));
            return comparison != 0 ? comparison < 0 : a->GetFormID() < b->GetFormID();
        });
        for (std::size_t i = 0; i < hiddenQuestList.size(); ++i) {
            if (i > 0) {
                output += ",";
            }
            output += "{\"id\":";
            AppendJsonString(output, std::format("{:08X}", hiddenQuestList[i]->GetFormID()));
            output += ",\"title\":";
            AppendJsonString(output, QuestName(hiddenQuestList[i]));
            output += "}";
        }
        output += "]}";
        return output;
    }

    void SendJournalData();

    void SaveQuestCategories(const char* payload)
    {
        if (!payload) {
            logger::warn("AdventurersJournal: missing quest category preferences");
            return;
        }
        try {
            const auto data = json::parse(payload);
            if (!data.is_object()) {
                logger::warn("AdventurersJournal: invalid quest category preferences");
                return;
            }
            static constexpr std::array keys{
                "main", "companions", "college", "thieves", "brotherhood",
                "civilWar", "side", "chronicles", "tasks"
            };
            json preferences = json::object();
            for (const auto key : keys) {
                const auto it = data.find(key);
                if (it != data.end()) {
                    if (!it->is_boolean()) {
                        logger::warn("AdventurersJournal: invalid quest category preference '{}'", key);
                        return;
                    }
                    preferences[key] = *it;
                }
            }
            AtomicWriteJson(UserDataDirectory() / "quest-categories.json", preferences);
        } catch (const json::exception& error) {
            logger::warn("AdventurersJournal: rejected quest category preferences: {}", error.what());
        }
    }

    void SaveJournalUserData(const char* payload)
    {
        if (!payload) {
            return;
        }
        try {
            const auto data = json::parse(payload);
            if (data.contains("fieldNotes") && data.at("fieldNotes").is_array()) {
                std::vector<std::string> notes;
                for (const auto& note : data.at("fieldNotes")) {
                    if (note.is_string() && notes.size() < kMaxNoteCount) {
                        notes.push_back(note.get<std::string>());
                    }
                }
                std::scoped_lock lock(questTimesMutex);
                if (saveReady.load() && data.value("saveGeneration", std::uint64_t{ 0 }) == saveGeneration.load()) {
                    fieldNotes = std::move(notes);
                } else {
                    logger::warn("AdventurersJournal: ignored field notes from an inactive save");
                }
            }
            if (data.contains("sketchMetadata")) {
                AtomicWriteJson(UserDataDirectory() / "sketch-metadata.json", data.at("sketchMetadata"));
            }
        } catch (const json::exception& error) {
            logger::warn("AdventurersJournal: rejected user-data save: {}", error.what());
        }
    }

    std::optional<json> ReadTrackingRequest(const char* payload)
    {
        if (!payload) {
            logger::warn("AdventurersJournal: missing quest tracking request");
            return std::nullopt;
        }
        try {
            auto request = json::parse(payload);
            if (!request.is_object() || !request.contains("saveGeneration") ||
                !request.at("saveGeneration").is_number_unsigned()) {
                logger::warn("AdventurersJournal: invalid quest tracking request");
                return std::nullopt;
            }
            return request;
        } catch (const json::exception& error) {
            logger::warn("AdventurersJournal: invalid quest tracking request: {}", error.what());
            return std::nullopt;
        }
    }

    struct QuestRequest
    {
        RE::FormID formID;
        std::uint64_t generation;
    };

    std::optional<QuestRequest> ReadQuestRequest(const char* payload)
    {
        const auto request = ReadTrackingRequest(payload);
        if (!request || !request->contains("id") || !request->at("id").is_string()) {
            if (request) {
                logger::warn("AdventurersJournal: missing quest FormID in tracking request");
            }
            return std::nullopt;
        }
        const auto formIDHex = request->at("id").get<std::string>();
        const auto generation = request->at("saveGeneration").get<std::uint64_t>();
        if (!saveReady.load() || generation != saveGeneration.load()) {
            logger::warn("AdventurersJournal: ignored quest tracking request from an inactive save");
            return std::nullopt;
        }
        if (formIDHex.size() != 8 || !std::all_of(formIDHex.begin(), formIDHex.end(), [](unsigned char ch) {
                return std::isxdigit(ch) != 0;
            })) {
            logger::warn("AdventurersJournal: invalid quest FormID in tracking request");
            return std::nullopt;
        }
        const RE::FormID formID = static_cast<RE::FormID>(std::strtoul(formIDHex.c_str(), nullptr, 16));
        if (formID == 0) {
            logger::warn("AdventurersJournal: invalid zero quest FormID");
            return std::nullopt;
        }
        return QuestRequest{ formID, generation };
    }

    void ToggleQuestTracking(const char* payload)
    {
        const auto request = ReadQuestRequest(payload);
        if (!request) {
            return;
        }
        const auto taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            logger::warn("ToggleQuestTracking: task interface unavailable");
            return;
        }
        const auto [formID, generation] = *request;
        taskInterface->AddTask([formID, generation] {
            if (!saveReady.load() || generation != saveGeneration.load()) {
                return;
            }
            auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(formID);
            if (!quest || !quest->IsRunning() || quest->IsCompleted()) {
                logger::warn("ToggleQuestTracking: quest 0x{:08X} not found", formID);
                return;
            }
            const bool nowActive = !quest->IsActive();
            SetQuestTracked(quest, nowActive);
            logger::info("ToggleQuestTracking: quest '{}' 0x{:08X} tracked={}", quest->GetFullName(), formID, nowActive);
            SendJournalData();
        });
    }

    void SetQuestAbandoned(const char* payload, bool abandon)
    {
        const auto request = ReadQuestRequest(payload);
        if (!request) {
            return;
        }
        const auto taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            logger::warn("AdventurersJournal: task interface unavailable for abandoned quest request");
            return;
        }
        const auto [formID, generation] = *request;
        taskInterface->AddTask([formID, generation, abandon] {
            if (!saveReady.load() || generation != saveGeneration.load()) {
                logger::warn("AdventurersJournal: ignored abandoned quest request from an inactive save");
                return;
            }
            auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(formID);
            if (!quest || (abandon && (!quest->IsRunning() || quest->IsCompleted()))) {
                logger::warn("AdventurersJournal: cannot {} quest 0x{:08X}",
                    abandon ? "abandon" : "restore", formID);
                return;
            }
            {
                std::scoped_lock lock(questTimesMutex);
                if (!saveReady.load() || generation != saveGeneration.load()) {
                    logger::warn("AdventurersJournal: save changed during abandoned quest request");
                    return;
                }
                if (abandon) {
                    const auto it = questTimes.find(formID);
                    const bool savedTracked = it != questTimes.end() && it->second.hasTracked && it->second.tracked;
                    if (quest->IsActive() || savedTracked) {
                        SetQuestTracked(quest, false);
                    }
                    auto& times = questTimes[formID];
                    times.hasTracked = true;
                    times.tracked = false;
                    abandonedQuests.insert(formID);
                } else {
                    abandonedQuests.erase(formID);
                }
            }
            SendJournalData();
        });
    }

    void ClearAllQuestTracking(const char* payload)
    {
        const auto request = ReadTrackingRequest(payload);
        if (!request) {
            return;
        }
        const auto generation = request->at("saveGeneration").get<std::uint64_t>();
        if (!saveReady.load() || generation != saveGeneration.load()) {
            logger::warn("AdventurersJournal: ignored untrack request from an inactive save");
            return;
        }
        const auto taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            logger::warn("ClearAllQuestTracking: task interface unavailable");
            return;
        }

        taskInterface->AddTask([generation] {
            if (!saveReady.load() || generation != saveGeneration.load()) {
                return;
            }
            const auto dataHandler = RE::TESDataHandler::GetSingleton();
            if (dataHandler) {
                for (auto* quest : dataHandler->GetFormArray<RE::TESQuest>()) {
                    if (quest && quest->IsActive()) {
                        quest->data.flags.set(false, RE::QuestFlag::kActive);
                        quest->AddChange(RE::TESQuest::ChangeFlags::kQuestFlags);
                    }
                }
            }

            if (const auto player = RE::PlayerCharacter::GetSingleton()) {
                player->GetPlayerFlags().forceQuestTargetRepath = true;
            }
            SendJournalData();
        });
    }

    void CloseJournal()
    {
        if (!journalOpen) {
            return;
        }

        if (journalPauseOwned) {
            if (const auto ui = RE::UI::GetSingleton()) {
                if (ui->numPausesGame > 0) {
                    --ui->numPausesGame;
                } else {
                    logger::error("AdventurersJournal: owned pause count was already zero");
                }
            } else {
                logger::error("AdventurersJournal: UI unavailable while releasing journal pause");
            }
            journalPauseOwned = false;
        }
        isTyping = false;
        SetTextInputActive(false);
        PlayJournalSound(kSoundClose);
        if (prismaUI) {
            prismaUI->Invoke(journalView, "window.onJournalClose()");
            prismaUI->Unfocus(journalView);
            prismaUI->Hide(journalView);
        }
        journalOpen = false;
    }

    void ReportQuestMapError(const char* message)
    {
        logger::warn("AdventurersJournal: {}", message);
        if (journalViewReady && prismaUI && prismaUI->IsValid(journalView)) {
            prismaUI->Invoke(journalView, ("window.showQuestMapError(" + json(message).dump() + ")").c_str());
        }
    }

    RE::ObjectRefHandle ResolveQuestMapTarget(RE::TESQuestTarget* questTarget, const RE::TESQuest* quest)
    {
        RE::ObjectRefHandle destination;
        questTarget->GetTargetRef(destination, true, quest);
        if (!destination.get()) {
            return {};
        }

        using MapTrackingRef = RE::ObjectRefHandle& (*)(RE::ObjectRefHandle&, RE::ObjectRefHandle&,
            const RE::TeleportPath*, std::uint32_t, bool);
        static REL::Relocation<MapTrackingRef> getMapTrackingRef{ RELOCATION_ID(52183, 53075) };
        constexpr std::uint32_t worldMapScope = 0;
        RE::ObjectRefHandle mapTarget;
        getMapTrackingRef(mapTarget, destination, &questTarget->teleportPath, worldMapScope, true);
        return mapTarget.get() ? mapTarget : RE::ObjectRefHandle{};
    }

    void FindQuestOnMap(const char* payload)
    {
        const auto request = ReadQuestRequest(payload);
        if (!request) {
            ReportQuestMapError("Unable to find this quest on the map.");
            return;
        }
        const auto tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            ReportQuestMapError("Unable to open the map right now.");
            return;
        }
        const auto [formID, generation] = *request;
        tasks->AddTask([formID, generation] {
            if (!saveReady.load() || generation != saveGeneration.load() || !journalOpen) {
                logger::warn("AdventurersJournal: ignored stale find-on-map request");
                return;
            }
            auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(formID);
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!quest || !player || !quest->IsRunning() || quest->IsCompleted()) {
                ReportQuestMapError("This quest is no longer active.");
                return;
            }
            if (REL::Module::IsVR()) {
                ReportQuestMapError("Finding quest targets on the map is not supported on Skyrim VR.");
                return;
            }
            {
                std::scoped_lock lock(questTimesMutex);
                if (abandonedQuests.contains(formID)) {
                    ReportQuestMapError("This quest has been abandoned.");
                    return;
                }
            }

            RE::ObjectRefHandle target;
            for (const auto objective : quest->objectives) {
                if (!objective || objective->state.get() != RE::QUEST_OBJECTIVE_STATE::kDisplayed) {
                    continue;
                }
                for (std::uint32_t i = 0; objective->targets && i < objective->numTargets; ++i) {
                    const auto questTarget = objective->targets[i];
                    if (!questTarget || !questTarget->conditions.IsTrue(player, nullptr)) {
                        continue;
                    }
                    target = ResolveQuestMapTarget(questTarget, quest);
                    if (target) {
                        break;
                    }
                }
                if (target) {
                    break;
                }
            }
            if (!target) {
                ReportQuestMapError("No map target is available for this quest.");
                return;
            }

            const auto queue = RE::UIMessageQueue::GetSingleton();
            const auto manager = RE::MessageDataFactoryManager::GetSingleton();
            const auto creator = manager ? manager->GetCreator<RE::RefHandleUIData>(RE::RefHandleUIData::CLASS_NAME) : nullptr;
            if (!queue || !creator) {
                ReportQuestMapError("Unable to open the map right now.");
                return;
            }
            auto* data = creator->Create();
            if (!data) {
                ReportQuestMapError("Unable to open the map right now.");
                return;
            }
            data->data = target.native_handle();
            SetQuestTracked(quest, true);
            CloseJournal();
            queue->AddMessage(RE::BSFixedString(RE::MapMenu::MENU_NAME), RE::UI_MESSAGE_TYPE::kShow, data);
        });
    }

    void SendJournalData()
    {
        if (!journalViewReady || !prismaUI || !prismaUI->IsValid(journalView)) {
            return;
        }

        const auto journalScript = "window.updateJournalFromSkyrim(" + BuildJournalJson() + ")";
        prismaUI->Invoke(journalView, journalScript.c_str());
    }

    void OpenJournal()
    {
        if (journalOpen || !prismaUI || !journalViewReady || !prismaUI->IsValid(journalView)) {
            return;
        }

        prismaUI->Show(journalView);
        if (!prismaUI->Focus(journalView, false, false)) {
            logger::warn("Failed to focus Adventurer's Journal view");
            prismaUI->Hide(journalView);
            return;
        }

        journalOpen = true;
        if (settings.pauseGameWhenOpen) {
            if (const auto ui = RE::UI::GetSingleton()) {
                ++ui->numPausesGame;
                journalPauseOwned = true;
            } else {
                logger::error("AdventurersJournal: cannot pause; UI is unavailable");
            }
        }
        prismaUI->SetOrder(journalView, 1000);
        logger::info("Adventurer's Journal focus state: {}", prismaUI->HasFocus(journalView));
        PlayJournalSound(kSoundOpen);
        SendJournalData();
        SendJournalUserData();
        prismaUI->Invoke(journalView, "window.onJournalOpen()");
    }

    class JournalInputSink final : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(
            RE::InputEvent* const* eventList,
            RE::BSTEventSource<RE::InputEvent*>*) override
        {
            const auto wasOpen = journalOpen;
            for (auto event = eventList ? *eventList : nullptr; event; event = event->next) {
                if (event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) {
                    continue;
                }

                const auto button = event->AsButtonEvent();
                if (!button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard || !button->IsDown()) {
                    continue;
                }

                const auto isJournalInput = IsJournalUserEvent(event) || button->GetIDCode() == 0x24;
                const auto isEscape = journalOpen && button->GetIDCode() == 0x01;
                if (!isJournalInput && !isEscape) {
                    continue;
                }

                if (journalOpen && isJournalInput && (isTyping || IsControlMapTextInputActive())) {
                    break;
                }

                if (!journalOpen && !CanOpenJournal()) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                CloseVanillaJournal();
                if (isEscape || journalOpen) {
                    CloseJournal();
                } else {
                    OpenJournal();
                }
                return RE::BSEventNotifyControl::kStop;
            }

            // A text field is focused: keep keyboard input away from other mods' hotkeys.
            if (wasOpen && journalOpen && isTyping && eventList && *eventList) {
                StripKeyboardEvents(const_cast<RE::InputEvent**>(eventList));
                if (!*eventList) {
                    return RE::BSEventNotifyControl::kStop;
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    JournalInputSink journalInputSink;

    // Deferred one frame so objectives and the completed flag have settled before stamping.
    void ReconcileQuestLater(RE::FormID formID)
    {
        if (!saveReady.load()) {
            return;
        }
        const auto taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            return;
        }
        const auto generation = saveGeneration.load();
        taskInterface->AddTask([formID, generation] {
            if (!saveReady.load() || generation != saveGeneration.load()) {
                return;
            }
            if (const auto quest = RE::TESForm::LookupByID<RE::TESQuest>(formID)) {
                ReconcileQuestTimes(quest, RE::PlayerCharacter::GetSingleton(), false, true);
            }
            if (journalOpen) {
                SendJournalData();
            }
        });
    }

    // Push updated quest data whenever any quest stage advances.
    class QuestStageSink final : public RE::BSTEventSink<RE::TESQuestStageEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESQuestStageEvent* event,
            RE::BSTEventSource<RE::TESQuestStageEvent>*) override
        {
            if (event) {
                ReconcileQuestLater(event->formID);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    QuestStageSink questStageSink;

    class QuestStartStopSink final : public RE::BSTEventSink<RE::TESQuestStartStopEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESQuestStartStopEvent* event,
            RE::BSTEventSource<RE::TESQuestStartStopEvent>*) override
        {
            if (event) {
                ReconcileQuestLater(event->formID);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    QuestStartStopSink questStartStopSink;
}

static void SKSEMessageHandler(SKSE::MessagingInterface::Message* message)
{
    if (!message) {
        return;
    }

    if (message->type == SKSE::MessagingInterface::kPreLoadGame ||
        message->type == SKSE::MessagingInterface::kNewGame) {
        saveReady.store(false);
        ++saveGeneration;
        ClearSaveState();
        CloseJournal();
        if (journalViewReady && prismaUI && prismaUI->IsValid(journalView)) {
            prismaUI->Invoke(journalView, "window.resetJournalSaveState()");
        }
        if (message->type == SKSE::MessagingInterface::kPreLoadGame) {
            return;
        }
        saveReady.store(true);
    } else if (message->type == SKSE::MessagingInterface::kPostLoadGame) {
        // SKSE passes the boolean result as the pointer value, not a bool pointer.
        if (!message->data) {
            ClearSaveState();
            logger::warn("AdventurersJournal: save load failed; quest state remains unavailable");
            return;
        }
        saveReady.store(true);
        RestoreQuestTracking();
    } else if (message->type != SKSE::MessagingInterface::kDataLoaded) {
        return;
    }

    if (message->type != SKSE::MessagingInterface::kDataLoaded) {
        InitializeQuestTimes();
        return;
    }

    prismaUI = static_cast<PRISMA_UI_API::IVPrismaUI1*>(
        PRISMA_UI_API::RequestPluginAPI(PRISMA_UI_API::InterfaceVersion::V1));
    if (!prismaUI) {
        logger::critical("Failed to acquire PrismaUI API.");
        return;
    }

    journalView = prismaUI->CreateView("AdventurersJournal/index.html", [](PrismaView a_view) {
        logger::info("Adventurer's Journal view is ready {}", a_view);
        journalViewReady = true;
        prismaUI->Hide(a_view);
        prismaUI->SetOrder(a_view, 1000);
        SendJournalData();
    });
    prismaUI->Hide(journalView);

    prismaUI->RegisterJSListener(journalView, "requestJournalClose", [](const char*) {
        if (const auto tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([] { CloseJournal(); });
        } else {
            logger::error("AdventurersJournal: cannot close journal; task interface is unavailable");
        }
    });
    prismaUI->RegisterJSListener(journalView, "playJournalSound", [](const char*) {
        PlayJournalSound(kSoundPageTurn);
    });
    prismaUI->RegisterJSListener(journalView, "setTypingState", [](const char* state) {
        isTyping = journalOpen && state && (std::strcmp(state, "true") == 0 || std::strcmp(state, "1") == 0);
        SetTextInputActive(isTyping);
    });
    prismaUI->RegisterJSListener(journalView, "toggleQuestTracking", [](const char* payload) {
        ToggleQuestTracking(payload);
    });
    prismaUI->RegisterJSListener(journalView, "clearAllQuestTracking", [](const char* payload) {
        ClearAllQuestTracking(payload);
    });
    prismaUI->RegisterJSListener(journalView, "abandonQuest", [](const char* payload) {
        SetQuestAbandoned(payload, true);
    });
    prismaUI->RegisterJSListener(journalView, "restoreQuest", [](const char* payload) {
        SetQuestAbandoned(payload, false);
    });
    prismaUI->RegisterJSListener(journalView, "findQuestOnMap", [](const char* payload) {
        FindQuestOnMap(payload);
    });
    prismaUI->RegisterJSListener(journalView, "requestJournalUserData", [](const char*) {
        SendJournalUserData();
    });
    prismaUI->RegisterJSListener(journalView, "requestJournalScreenshots", [](const char*) {
        SendJournalScreenshots();
    });
    prismaUI->RegisterJSListener(journalView, "saveJournalUserData", [](const char* payload) {
        SaveJournalUserData(payload);
    });
    prismaUI->RegisterJSListener(journalView, "saveQuestCategories", [](const char* payload) {
        SaveQuestCategories(payload);
    });

    if (const auto inputManager = RE::BSInputDeviceManager::GetSingleton()) {
        inputManager->PrependEventSink(&journalInputSink);
    } else {
        logger::critical("Failed to get InputDeviceManager. Journal input sink was not registered.");
    }

    if (const auto scriptEvents = RE::ScriptEventSourceHolder::GetSingleton()) {
        scriptEvents->AddEventSink<RE::TESQuestStageEvent>(&questStageSink);
        scriptEvents->AddEventSink<RE::TESQuestStartStopEvent>(&questStartStopSink);
    } else {
        logger::critical("Failed to get ScriptEventSourceHolder. Quest stage sink was not registered.");
    }
}

extern "C" DLLEXPORT bool SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    REL::Module::reset();

    auto messaging = reinterpret_cast<SKSE::MessagingInterface*>(
        a_skse->QueryInterface(SKSE::LoadInterface::kMessaging));
    if (!messaging) {
        logger::critical("Failed to load messaging interface! This error is fatal, plugin will not load.");
        return false;
    }

    SKSE::Init(a_skse, { .trampoline = true, .trampolineSize = 1 << 10 });
    LoadSettings();
    if (const auto serialization = SKSE::GetSerializationInterface()) {
        serialization->SetUniqueID(kSerializationUID);
        serialization->SetSaveCallback(SaveCosave);
        serialization->SetLoadCallback(LoadCosave);
        serialization->SetRevertCallback(RevertCosave);
    } else {
        logger::critical("Serialization interface unavailable; refusing to load without per-save quest state.");
        return false;
    }
    if (!messaging->RegisterListener("SKSE", SKSEMessageHandler)) {
        logger::critical("Failed to register SKSE message listener.");
        return false;
    }
    return true;
}
