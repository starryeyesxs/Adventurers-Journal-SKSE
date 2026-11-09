#include "PrismaUI_API.h"
#include <RE/B/BGSRefAlias.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>

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

    void SendJournalUserData()
    {
        if (!journalViewReady || !prismaUI || !prismaUI->IsValid(journalView)) {
            return;
        }

        const auto userData = UserDataDirectory();
        const bool notesExist = fs::exists(userData / "field-notes.json");
        const bool metadataExist = fs::exists(userData / "sketch-metadata.json");
        const auto notes = ReadJson(userData / "field-notes.json", json::array());
        const auto metadata = ReadJson(userData / "sketch-metadata.json", json::object());
        prismaUI->Invoke(journalView, ("window.loadNativeUserData(" + json{
            { "fieldNotes", notes },
            { "sketchMetadata", metadata },
            { "fieldNotesExists", notesExist },
            { "sketchMetadataExists", metadataExist }
        }.dump() + ")").c_str());
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

    bool IsJournalBlocked()
    {
        const auto ui = RE::UI::GetSingleton();
        return ui && (ui->IsMenuOpen("Main Menu") || ui->IsMenuOpen("Loading Menu"));
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
        std::string resolved;
        resolved.reserve(text.size());

        const auto equalsIgnoreCase = [](std::string_view left, std::string_view right) {
            if (left.size() != right.size()) {
                return false;
            }
            for (std::size_t index = 0; index < left.size(); ++index) {
                if (std::tolower(static_cast<unsigned char>(left[index])) !=
                    std::tolower(static_cast<unsigned char>(right[index]))) {
                    return false;
                }
            }
            return true;
        };

        std::size_t cursor = 0;
        while (cursor < text.size()) {
            const auto tokenStart = text.find('<', cursor);
            if (tokenStart == std::string_view::npos) {
                resolved.append(text.substr(cursor));
                break;
            }

            resolved.append(text.substr(cursor, tokenStart - cursor));
            const auto tokenEnd = text.find('>', tokenStart + 1);
            if (tokenEnd == std::string_view::npos) {
                resolved.append(text.substr(tokenStart));
                break;
            }

            const auto token = text.substr(tokenStart + 1, tokenEnd - tokenStart - 1);
            const auto separator = token.find('=');
            const auto tokenType = separator == std::string_view::npos
                                       ? token
                                       : token.substr(0, separator);
            if (!equalsIgnoreCase(tokenType, "Alias") &&
                !equalsIgnoreCase(tokenType, "Alias.CurrentName")) {
                resolved.append(text.substr(tokenStart, tokenEnd - tokenStart + 1));
                cursor = tokenEnd + 1;
                continue;
            }

            const auto aliasName = separator == std::string_view::npos
                                       ? std::string_view{}
                                       : token.substr(separator + 1);
            std::string replacement;
            bool aliasFound = false;
            bool referenceFound = false;
            if (quest) {
                for (const auto alias : quest->aliases) {
                    if (!alias || !alias->aliasName.c_str() ||
                        !equalsIgnoreCase(alias->aliasName.c_str(), aliasName)) {
                        continue;
                    }

                    aliasFound = true;
                    RE::TESObjectREFR* reference = nullptr;
                    const auto refIt = quest->refAliasMap.find(alias->aliasID);
                    if (refIt != quest->refAliasMap.end() && refIt->second) {
                        referenceFound = true;
                        const auto resolvedReference = refIt->second.get();
                        reference = resolvedReference.get();
                    }

                    if (!reference) {
                        const auto refAlias = dynamic_cast<RE::BGSRefAlias*>(alias);
                        reference = refAlias ? refAlias->GetReference() : nullptr;
                        referenceFound = reference != nullptr;
                    }

                    if (reference) {
                        if (const auto name = reference->GetName(); name && *name != '\0') {
                            replacement = name;
                        } else if (const auto form = reference->GetObjectReference();
                                   form && form->GetName() && *form->GetName() != '\0') {
                            replacement = form->GetName();
                        }
                    }
                    break;
                }
            }

            if (replacement.empty()) {
                resolved.append(text.substr(tokenStart, tokenEnd - tokenStart + 1));
                const auto questName = quest && quest->GetFullName() ? quest->GetFullName() : "<unnamed>";
                logger::warn(
                    "Quest text replacement unresolved: quest='{}', alias='{}', reason={}",
                    questName,
                    aliasName,
                    !quest ? "quest unavailable" : !aliasFound ? "alias not found" :
                    !referenceFound ? "alias unfilled" : "reference has no display name");
            } else {
                resolved += replacement;
            }
            cursor = tokenEnd + 1;
        }

        return resolved;
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

    void AppendQuest(std::string& output, RE::TESQuest* quest, RE::PlayerCharacter* player, std::string_view currentDate)
    {
        output += "{\"id\":";
        AppendJsonString(output, std::format("{:08X}", quest->GetFormID()));
        output += ",\"tracked\":";
        output += quest->IsActive() ? "true" : "false";
        output += ",\"title\":";
        AppendJsonString(output, QuestName(quest));
        output += ",\"date\":";
        AppendJsonString(output, currentDate);
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

    std::string BuildJournalJson()
    {
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
        output += ",\"active\":[";
        bool firstActive = true;
        for (const auto quest : dataHandler->GetFormArray<RE::TESQuest>()) {
            if (!ShouldShowQuest(quest, player)) {
                continue;
            }
            if (quest->IsCompleted()) {
                continue;  // completed always goes to completed list only
            }
            if (!quest->IsRunning() || quest->IsCompleted()) {
                continue;
            }
            if (!firstActive) {
                output += ",";
            }
            AppendQuest(output, quest, player, currentDate);
            firstActive = false;
        }

        output += "],\"completed\":[";
        bool firstCompleted = true;
        for (const auto quest : dataHandler->GetFormArray<RE::TESQuest>()) {
            if (!ShouldShowQuest(quest, player)) {
                continue;
            }
            if (!quest->IsCompleted()) {
                continue;
            }
            if (!firstCompleted) {
                output += ",";
            }
            AppendQuest(output, quest, player, currentDate);
            firstCompleted = false;
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
            const auto userData = UserDataDirectory();
            if (data.contains("fieldNotes")) {
                AtomicWriteJson(userData / "field-notes.json", data.at("fieldNotes"));
            }
            if (data.contains("sketchMetadata")) {
                AtomicWriteJson(userData / "sketch-metadata.json", data.at("sketchMetadata"));
            }
        } catch (const json::exception& error) {
            logger::warn("AdventurersJournal: rejected user-data save: {}", error.what());
        }
    }

    // Toggle the kActive tracking flag on the quest identified by its hex FormID string.
    // The HUD compass is updated automatically when kActive changes.
    void ToggleQuestTracking(const char* formIDHex)
    {
        if (!formIDHex || *formIDHex == '\0') {
            return;
        }
        const auto taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            return;
        }
        // Parse hex FormID from JS (e.g. "0003CABB")
        const RE::FormID formID = static_cast<RE::FormID>(std::strtoul(formIDHex, nullptr, 16));
        if (formID == 0) {
            return;
        }
        taskInterface->AddTask([formID] {
            const auto dataHandler = RE::TESDataHandler::GetSingleton();
            auto* quest = dataHandler ? dataHandler->LookupForm<RE::TESQuest>(formID, "Skyrim.esm") : nullptr;
            // Also search all loaded mods if not in Skyrim.esm
            if (!quest) {
                quest = RE::TESForm::LookupByID<RE::TESQuest>(formID);
            }
            if (!quest) {
                logger::warn("ToggleQuestTracking: quest 0x{:08X} not found", formID);
                return;
            }
            const bool nowActive = !quest->IsActive();
            quest->data.flags.set(nowActive, RE::QuestFlag::kActive);
            logger::info("ToggleQuestTracking: quest '{}' 0x{:08X} tracked={}", quest->GetFullName(), formID, nowActive);
            SendJournalData();
        });
    }

    void ClearAllQuestTracking()
    {
        const auto taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            logger::warn("ClearAllQuestTracking: task interface unavailable");
            return;
        }

        taskInterface->AddTask([] {
            const auto dataHandler = RE::TESDataHandler::GetSingleton();
            if (dataHandler) {
                for (auto* quest : dataHandler->GetFormArray<RE::TESQuest>()) {
                    if (quest && quest->IsActive()) {
                        quest->data.flags.set(false, RE::QuestFlag::kActive);
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

                if (isTyping && isJournalInput && !isEscape) {
                    continue;
                }

                CloseVanillaJournal();
                if (isEscape || journalOpen) {
                    CloseJournal();
                } else if (!IsJournalBlocked()) {
                    OpenJournal();
                }
                return RE::BSEventNotifyControl::kStop;
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    JournalInputSink journalInputSink;

    // Push updated quest data whenever any quest stage advances.
    class QuestStageSink final : public RE::BSTEventSink<RE::TESQuestStageEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESQuestStageEvent*,
            RE::BSTEventSource<RE::TESQuestStageEvent>*) override
        {
            if (journalOpen) {
                SendJournalData();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    QuestStageSink questStageSink;
}

static void SKSEMessageHandler(SKSE::MessagingInterface::Message* message)
{
    if (!message || message->type != SKSE::MessagingInterface::kDataLoaded) {
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
        isTyping = state && (std::strcmp(state, "true") == 0 || std::strcmp(state, "1") == 0);
    });
    prismaUI->RegisterJSListener(journalView, "toggleQuestTracking", [](const char* formIDHex) {
        ToggleQuestTracking(formIDHex);
    });
    prismaUI->RegisterJSListener(journalView, "clearAllQuestTracking", [](const char*) {
        ClearAllQuestTracking();
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
    messaging->RegisterListener("SKSE", SKSEMessageHandler);
    return true;
}
