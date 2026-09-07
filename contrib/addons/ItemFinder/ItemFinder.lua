-- ItemFinder
--
-- A window to type an item name into, and the matching item IDs back in a box you can select and
-- copy out of. The lookup itself is the server's own ".lookup item" command: this sends it, catches
-- the reply out of the system chat stream before the default chat frame prints it, and shows the
-- results here instead of leaving fifty item links scrolling through chat.
--
-- Written for the 1.12 client, so: Lua 5.0 (string.find with captures, no string.match), event
-- handlers read the globals "this" and "arg1", and there is no ChatFrame_AddMessageEventFilter,
-- which is why the suppression below is a hook on ChatFrame_OnEvent instead.

local MAX_RESULTS_KEPT = 200

-- How long to keep listening after the last line arrives. The server sends one chat message per
-- match and they do not all land in the same frame, so a search is finished by going quiet rather
-- than by any single message - except for the two lines that really are the end, which cut it short.
local QUIET_TIMEOUT = 2.5
local TERMINAL_GRACE = 0.3

local ItemFinder = {}
ItemFinder.capturing = false
ItemFinder.results = {}
ItemFinder.resultCount = 0
ItemFinder.shownCount = 0
ItemFinder.deadline = 0
ItemFinder.note = nil
ItemFinder.query = ""

local function Trim(text)
    if not text then
        return ""
    end

    local _, _, trimmed = string.find(text, "^%s*(.-)%s*$")
    return trimmed or ""
end

----------------------------------------------------------------------------------------------------
-- Reading the server's reply
----------------------------------------------------------------------------------------------------

-- Pull an id and a name out of one line of ".lookup item" output.
--
-- The id is taken from the hyperlink rather than from the number the line starts with. Both are
-- there and they are the same number, but the link is the part the server builds from the item
-- itself, so it survives any change to how the line is worded.
local function ParseResultLine(message)
    if not message then
        return nil
    end

    local _, _, id = string.find(message, "Hitem:(%d+):")
    if not id then
        return nil
    end

    local _, _, name = string.find(message, "%[(.-)%]")
    if not name then
        name = "?"
    end

    return id, name
end

local function IsTerminalLine(message)
    if not message then
        return false
    end

    local lower = string.lower(message)

    if string.find(lower, "^showing %d+ of %d+") then
        return true
    end

    -- Worded by the server's string table rather than here, so matched loosely on purpose.
    if string.find(lower, "no item") and string.find(lower, "found") then
        return true
    end

    return false
end

-- Returns true when the line belonged to this search, which is also the signal to keep it out of
-- the default chat frame.
function ItemFinder:Consume(message)
    if not self.capturing then
        return false
    end

    if IsTerminalLine(message) then
        self.note = message
        self.deadline = GetTime() + TERMINAL_GRACE
        return true
    end

    local id, name = ParseResultLine(message)
    if not id then
        return false
    end

    self.resultCount = self.resultCount + 1

    if self.shownCount < MAX_RESULTS_KEPT then
        self.shownCount = self.shownCount + 1
        self.results[self.shownCount] = { id = id, name = name }
    end

    self.deadline = GetTime() + QUIET_TIMEOUT
    return true
end

function ItemFinder:Finish()
    if not self.capturing then
        return
    end

    self.capturing = false
    self:Render()
end

----------------------------------------------------------------------------------------------------
-- The window
----------------------------------------------------------------------------------------------------

function ItemFinder:SetStatus(text)
    if self.status then
        self.status:SetText(text or "")
    end
end

function ItemFinder:Render()
    local lines = {}
    local count = table.getn(self.results)

    for i = 1, count do
        local entry = self.results[i]
        lines[i] = entry.id .. "    " .. entry.name
    end

    local body = table.concat(lines, "\n")
    self.output:SetText(body)
    self.scroll:SetVerticalScroll(0)

    if self.resultCount == 0 then
        self:SetStatus("No items match \"" .. self.query .. "\".")
    elseif self.resultCount > count then
        self:SetStatus(count .. " of " .. self.resultCount .. " matches for \"" .. self.query ..
                       "\". Narrow the search to see the rest.")
    else
        self:SetStatus(count .. " match(es) for \"" .. self.query .. "\".")
    end
end

function ItemFinder:Search(text)
    text = Trim(text)
    if text == "" then
        return
    end

    self.query = text
    self.results = {}
    self.resultCount = 0
    self.shownCount = 0
    self.note = nil
    self.capturing = true
    self.deadline = GetTime() + QUIET_TIMEOUT

    self.output:SetText("")
    self:SetStatus("Searching for \"" .. text .. "\" ...")

    -- The server reads a leading dot off an ordinary say as a command, so this never reaches
    -- anybody's chat. It does need the account to hold moderator rights or better; without them the
    -- server answers with a permissions error and the window simply finds nothing.
    SendChatMessage(".lookup item " .. text, "SAY")
end

function ItemFinder:Toggle()
    if self.frame:IsVisible() then
        self.frame:Hide()
    else
        self.frame:Show()
        self.input:SetFocus()
    end
end

local function BuildWindow()
    local frame = CreateFrame("Frame", "ItemFinderFrame", UIParent)
    frame:SetWidth(460)
    frame:SetHeight(380)
    frame:SetPoint("CENTER", UIParent, "CENTER", 0, 0)
    frame:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 32, edgeSize = 16,
        insets = { left = 5, right = 5, top = 5, bottom = 5 },
    })
    frame:SetMovable(true)
    frame:EnableMouse(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", function() this:StartMoving() end)
    frame:SetScript("OnDragStop", function() this:StopMovingOrSizing() end)
    frame:Hide()

    -- Escape closes it, the same as every other panel in the game.
    table.insert(UISpecialFrames, "ItemFinderFrame")

    local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    title:SetPoint("TOP", frame, "TOP", 0, -14)
    title:SetText("ItemFinder")

    local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -4, -4)

    local label = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    label:SetPoint("TOPLEFT", frame, "TOPLEFT", 18, -40)
    label:SetText("Item name")

    local input = CreateFrame("EditBox", "ItemFinderInput", frame, "InputBoxTemplate")
    input:SetPoint("TOPLEFT", frame, "TOPLEFT", 22, -56)
    input:SetWidth(300)
    input:SetHeight(20)
    input:SetAutoFocus(false)
    input:SetScript("OnEnterPressed", function()
        ItemFinder:Search(this:GetText())
    end)
    input:SetScript("OnEscapePressed", function()
        this:ClearFocus()
    end)

    local search = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    search:SetPoint("LEFT", input, "RIGHT", 10, 0)
    search:SetWidth(90)
    search:SetHeight(22)
    search:SetText("Search")
    search:SetScript("OnClick", function()
        ItemFinder:Search(ItemFinderInput:GetText())
    end)

    local status = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    status:SetPoint("TOPLEFT", frame, "TOPLEFT", 22, -84)
    status:SetWidth(410)
    status:SetJustifyH("LEFT")
    status:SetText("Type a name and press Enter.")

    -- The results live in an edit box rather than a message frame so the ids can be selected and
    -- copied out, which is the whole reason for wanting them on screen.
    local scroll = CreateFrame("ScrollFrame", "ItemFinderScroll", frame, "UIPanelScrollFrameTemplate")
    scroll:SetPoint("TOPLEFT", frame, "TOPLEFT", 22, -104)
    scroll:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -40, 46)

    local output = CreateFrame("EditBox", "ItemFinderOutput", scroll)
    output:SetMultiLine(true)
    output:SetAutoFocus(false)
    output:SetFontObject(GameFontHighlightSmall)
    output:SetWidth(370)
    output:SetHeight(600)
    output:SetScript("OnEscapePressed", function()
        this:ClearFocus()
    end)
    scroll:SetScrollChild(output)

    local selectAll = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    selectAll:SetPoint("BOTTOMLEFT", frame, "BOTTOMLEFT", 22, 16)
    selectAll:SetWidth(100)
    selectAll:SetHeight(22)
    selectAll:SetText("Select all")
    selectAll:SetScript("OnClick", function()
        ItemFinderOutput:SetFocus()
        ItemFinderOutput:HighlightText()
    end)

    local closeButton = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    closeButton:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -22, 16)
    closeButton:SetWidth(100)
    closeButton:SetHeight(22)
    closeButton:SetText("Close")
    closeButton:SetScript("OnClick", function()
        ItemFinderFrame:Hide()
    end)

    ItemFinder.frame = frame
    ItemFinder.input = input
    ItemFinder.status = status
    ItemFinder.scroll = scroll
    ItemFinder.output = output
end

----------------------------------------------------------------------------------------------------
-- Wiring
----------------------------------------------------------------------------------------------------

BuildWindow()

-- A search ends by going quiet, so something has to notice the quiet.
local ticker = CreateFrame("Frame")
ticker:SetScript("OnUpdate", function()
    if ItemFinder.capturing and GetTime() > ItemFinder.deadline then
        ItemFinder:Finish()
    end
end)

-- Swallow the reply before chat prints it.
--
-- The 1.12 client has no message filter API, so the only place to stand between the event and the
-- chat frame is ChatFrame_OnEvent itself. Only lines that parse as results of a search this addon
-- started are held back: anything else, including every system message while no search is running,
-- goes through untouched.
local ChatFrame_OnEvent_Original = ChatFrame_OnEvent
ChatFrame_OnEvent = function(event)
    if event == "CHAT_MSG_SYSTEM" and ItemFinder:Consume(arg1) then
        return
    end

    ChatFrame_OnEvent_Original(event)
end

SLASH_ITEMFINDER1 = "/itemfinder"
SLASH_ITEMFINDER2 = "/itemid"
SlashCmdList["ITEMFINDER"] = function(msg)
    local text = Trim(msg)

    ItemFinder.frame:Show()

    if text ~= "" then
        ItemFinder.input:SetText(text)
        ItemFinder:Search(text)
    else
        ItemFinder.input:SetFocus()
    end
end
