#include "PrismaUI_API.h"
#include "quest_text.h"
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
    namespace fs = std::filesystem;
    using json = nlohmann::json;

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
        prismaUI->Invoke(journalView, ("window.loadNativeUserData(" + json{
            { "fieldNotes", SnapshotFieldNotes() },
            { "saveGeneration", saveGeneration.load() },
            { "sketchMetadata", ReadJson(metadataPath, json::object()) },
            { "sketchMetadataExists", fs::exists(metadataPath) }
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
                    const auto refIt = quest->refAliasMap.find(alias->aliasID);
                    if (refIt != quest->refAliasMap.end() && refIt->second) {
                        const auto resolvedReference = refIt->second.get();
                        reference = resolvedReference.get();
                    }

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

    // Tamrielic accepted/completed stamps persisted in the SKSE cosave. Quests finished
    // before tracking existed have neither stamp and are treated as legacy entries.
    struct QuestTimes
    {
        bool hasAccepted = false;
        bool hasCompleted = false;
        bool hasTracked = false;
        bool tracked = false;
        float acceptedDays = 0.0f;
        float completedDays = 0.0f;
        std::uint64_t completionOrder = 0;
        std::string acceptedDate;
        std::string completedDate;
    };

    std::mutex questTimesMutex;
    std::unordered_map<RE::FormID, QuestTimes> questTimes;
    std::unordered_set<RE::FormID> observedActiveQuests;
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
            // Completed with no record: a legacy quest, deliberately left unstamped.
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

    void ReconcileAllQuestTimes()
    {
        if (!saveReady.load()) {
            return;
        }
        const auto player = RE::PlayerCharacter::GetSingleton();
        const auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!player || !dataHandler) {
            return;
        }
        for (const auto quest : dataHandler->GetFormArray<RE::TESQuest>()) {
            if (quest && (quest->IsCompleted() || quest->IsRunning())) {
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
    }

    void LoadCosave(SKSE::SerializationInterface* intfc)
    {
        std::unordered_map<RE::FormID, QuestTimes> loadedTimes;
        std::vector<std::string> loadedNotes;
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
            } else if (type == kQuestTimesRecord || type == kFieldNotesRecord) {
                logger::warn("AdventurersJournal: unsupported cosave record version {} for type 0x{:08X}",
                    version, type);
            }
        }

        std::scoped_lock lock(questTimesMutex);
        questTimes = std::move(loadedTimes);
        fieldNotes = std::move(loadedNotes);
        nextCompletionOrder = loadedCompletionOrder;
    }

    void ClearSaveState()
    {
        std::scoped_lock lock(questTimesMutex);
        questTimes.clear();
        observedActiveQuests.clear();
        fieldNotes.clear();
        nextCompletionOrder = 0;
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
            if (it != questTimes.end() && it->second.hasTracked && quest->IsActive() != it->second.tracked) {
                quest->data.flags.set(it->second.tracked, RE::QuestFlag::kActive);
                quest->AddChange(RE::TESQuest::ChangeFlags::kQuestFlags);
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

        // kCompleted takes strict precedence. A completed quest with kRunning still set
        // (common for post-quest dialogue) goes only to completed, never to active.
        std::vector<RE::TESQuest*> activeQuests;
        std::vector<RE::TESQuest*> completedQuests;
        for (const auto quest : dataHandler->GetFormArray<RE::TESQuest>()) {
            if (!ShouldShowQuest(quest, player)) {
                continue;
            }
            ReconcileQuestTimes(quest, player, true);
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
        output += "]}";
        return output;
    }

    void SendJournalData();

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

    void ToggleQuestTracking(const char* payload)
    {
        const auto request = ReadTrackingRequest(payload);
        if (!request || !request->contains("id") || !request->at("id").is_string()) {
            if (request) {
                logger::warn("AdventurersJournal: missing quest FormID in tracking request");
            }
            return;
        }
        const auto formIDHex = request->at("id").get<std::string>();
        const auto generation = request->at("saveGeneration").get<std::uint64_t>();
        if (!saveReady.load() || generation != saveGeneration.load()) {
            logger::warn("AdventurersJournal: ignored quest tracking request from an inactive save");
            return;
        }
        if (formIDHex.size() != 8 || !std::all_of(formIDHex.begin(), formIDHex.end(), [](unsigned char ch) {
                return std::isxdigit(ch) != 0;
            })) {
            logger::warn("AdventurersJournal: invalid quest FormID in tracking request");
            return;
        }
        const auto taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            logger::warn("ToggleQuestTracking: task interface unavailable");
            return;
        }
        const RE::FormID formID = static_cast<RE::FormID>(std::strtoul(formIDHex.c_str(), nullptr, 16));
        if (formID == 0) {
            logger::warn("ToggleQuestTracking: invalid zero quest FormID");
            return;
        }
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
            quest->data.flags.set(nowActive, RE::QuestFlag::kActive);
            quest->AddChange(RE::TESQuest::ChangeFlags::kQuestFlags);
            if (const auto player = RE::PlayerCharacter::GetSingleton()) {
                player->GetPlayerFlags().forceQuestTargetRepath = true;
            }
            logger::info("ToggleQuestTracking: quest '{}' 0x{:08X} tracked={}", quest->GetFullName(), formID, nowActive);
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
        if (!journalOpen || !prismaUI) {
            return;
        }

        isTyping = false;
        SetTextInputActive(false);
        PlayJournalSound(kSoundClose);
        prismaUI->Invoke(journalView, "window.onJournalClose()");
        prismaUI->Unfocus(journalView);
        prismaUI->Hide(journalView);
        journalOpen = false;
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
        if (journalViewReady && prismaUI && prismaUI->IsValid(journalView)) {
            CloseJournal();
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
        if (const auto taskInterface = SKSE::GetTaskInterface()) {
            const auto generation = saveGeneration.load();
            taskInterface->AddTask([generation] {
                if (saveReady.load() && generation == saveGeneration.load()) {
                    ReconcileAllQuestTimes();
                }
            });
        } else {
            logger::warn("AdventurersJournal: cannot reconcile quest timestamps; task interface unavailable");
        }
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
        CloseJournal();
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
    prismaUI->RegisterJSListener(journalView, "requestJournalUserData", [](const char*) {
        SendJournalUserData();
    });
    prismaUI->RegisterJSListener(journalView, "requestJournalScreenshots", [](const char*) {
        SendJournalScreenshots();
    });
    prismaUI->RegisterJSListener(journalView, "saveJournalUserData", [](const char* payload) {
        SaveJournalUserData(payload);
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
