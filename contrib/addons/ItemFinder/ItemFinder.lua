-- ItemFinder
--
-- Search an item by name, see it as an icon with a real tooltip, choose how many, put them in your
-- bag. Self-contained: the lookup is the server's own ".lookup item" and the delivery is its
-- ".additem", both sent as chat and both requiring the account to hold GM rights.
--
-- Written for the 1.12 client, so: Lua 5.0 (string.find with captures, no string.match, table.getn
-- rather than #), event handlers read the globals "this" and "arg1", and there is no
-- ChatFrame_AddMessageEventFilter, which is why the reply is intercepted by hooking
-- ChatFrame_OnEvent instead.
--
-- Two things here are deliberately defensive rather than direct, because guessing a client API and
-- being wrong shows up as a blank window rather than an error:
--
--   Quality colour is read out of the server's own reply. The reply is already a coloured
--   hyperlink built from the item, so the colour is in the text; taking it from there needs no
--   client-side quality table to exist under any particular name.
--
--   The icon path is found by looking through everything GetItemInfo returns for the one that
--   looks like a texture path, rather than by counting arguments. The 1.12 return list differs
--   from later ones, and a hardcoded position silently yields a stack count where a path belongs.

local ROWS = 8
local ROW_HEIGHT = 38
local MAX_RESULTS_KEPT = 300
local PLACEHOLDER_ICON = "Interface\\Icons\\INV_Misc_QuestionMark"

-- A search ends by going quiet: the server sends one chat message per match and they do not all
-- land in the same frame. The two lines that really are the end cut the wait short.
local QUIET_TIMEOUT = 2.5
local TERMINAL_GRACE = 0.3

-- Items the client has never seen have no local cache entry, so GetItemInfo answers nothing until
-- the server has been asked. Building the tooltip is what asks. Until the answer arrives the row
-- wears a question mark, and this is how long it keeps re-checking for the real icon.
local ICON_RETRY_WINDOW = 12.0
local ICON_RETRY_INTERVAL = 0.1

-- How far past the visible rows to ask the server for item data. Priming a page ahead means
-- scrolling lands on icons that are already there instead of on question marks that resolve a
-- moment later, without asking for all three hundred results at once.
local ICON_PRIME_AHEAD = ROWS * 2

local ItemFinder = {}
ItemFinder.capturing = false
ItemFinder.results = {}
ItemFinder.resultCount = 0
ItemFinder.shownCount = 0
ItemFinder.deadline = 0
ItemFinder.query = ""
ItemFinder.offset = 0
ItemFinder.iconRetryUntil = 0
ItemFinder.nextIconCheck = 0
ItemFinder.rows = {}

local function Trim(text)
    if not text then
        return ""
    end

    local _, _, trimmed = string.find(text, "^%s*(.-)%s*$")
    return trimmed or ""
end

local function ItemLink(id)
    return "item:" .. id .. ":0:0:0:0:0:0:0"
end

-- The eight hex digits the server sends with a coloured link are alpha, red, green, blue. Only the
-- last three matter here, and a missing or malformed colour falls back to a neutral grey rather
-- than to nothing, so a row without one still gets a border instead of a gap.
local function ColourToRGB(hex)
    if not hex or string.len(hex) < 8 then
        return 0.45, 0.45, 0.45
    end

    local r = tonumber(string.sub(hex, 3, 4), 16)
    local g = tonumber(string.sub(hex, 5, 6), 16)
    local b = tonumber(string.sub(hex, 7, 8), 16)

    if not r or not g or not b then
        return 0.45, 0.45, 0.45
    end

    return r / 255, g / 255, b / 255
end

----------------------------------------------------------------------------------------------------
-- Reading the server's reply
----------------------------------------------------------------------------------------------------

-- Pull an id, a name and the quality colour out of one line of ".lookup item" output.
--
-- The id comes from the hyperlink rather than from the number the line starts with. Both are there
-- and they are the same number, but the link is the part the server builds from the item itself, so
-- it survives any rewording of the line around it.
local function ParseResultLine(message)
    if not message then
        return nil
    end

    local _, _, id = string.find(message, "Hitem:(%d+):")
    if not id then
        return nil
    end

    local _, _, colour = string.find(message, "|c(%x%x%x%x%x%x%x%x)|Hitem:")
    local _, _, name = string.find(message, "%[(.-)%]")

    return id, name or ("Item " .. id), colour
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
        self.deadline = GetTime() + TERMINAL_GRACE
        return true
    end

    local id, name, colour = ParseResultLine(message)
    if not id then
        return false
    end

    self.resultCount = self.resultCount + 1

    if self.shownCount < MAX_RESULTS_KEPT then
        self.shownCount = self.shownCount + 1
        self.results[self.shownCount] = {
            id = id,
            name = name,
            colour = colour,
            count = 1,
        }
    end

    self.deadline = GetTime() + QUIET_TIMEOUT
    return true
end

function ItemFinder:Finish()
    if not self.capturing then
        return
    end

    self.capturing = false
    self.offset = 0
    self.iconRetryUntil = GetTime() + ICON_RETRY_WINDOW
    self.nextIconCheck = 0
    self:Refresh()
end

----------------------------------------------------------------------------------------------------
-- Item data from the client cache
----------------------------------------------------------------------------------------------------

-- Ask the server for an item the client has never seen.
--
-- Nothing in the API requests item data directly, but building a tooltip for an item does it as a
-- side effect, and a tooltip that is never shown works just as well. So the icons no longer wait
-- for the person to hover: every row in and just past view is asked for as soon as it is drawn,
-- which is the difference between icons appearing on their own and appearing only where the mouse
-- has already been.
local primer = CreateFrame("GameTooltip", "ItemFinderPrimer", UIParent, "GameTooltipTemplate")
primer:SetOwner(UIParent, "ANCHOR_NONE")
primer:Hide()

local primed = {}

local function PrimeItem(id)
    if primed[id] then
        return
    end

    primed[id] = true
    primer:SetOwner(UIParent, "ANCHOR_NONE")
    primer:SetHyperlink(ItemLink(id))
    primer:Hide()
end

-- Look through everything GetItemInfo gives back for the value that is a texture path.
--
-- The 1.12 return list is shorter than later ones and in a different order, so reading the icon by
-- argument position is a guess that fails quietly - it hands back a stack count and the row shows
-- no icon at all. Recognising the path by its shape works whatever the order.
local function ItemIconAndStack(id)
    local a, b, c, d, e, f, g, h, i, j = GetItemInfo(id)
    local returns = { a, b, c, d, e, f, g, h, i, j }

    local icon = nil
    local stack = nil

    for n = 1, 10 do
        local value = returns[n]

        if type(value) == "string" then
            -- A texture path, and specifically not the item link, which also contains a backslash
            -- free colon-separated payload but never a path separator.
            if string.find(value, "\\") then
                icon = value
            end
        elseif type(value) == "number" then
            -- Stack size is the only return that is plausibly a stack size: quality is 0-6 and
            -- required level is small, so take the largest sensible one and treat 1 as unknown.
            if value > 1 and value <= 200 and (not stack or value > stack) then
                stack = value
            end
        end
    end

    return icon, stack
end

----------------------------------------------------------------------------------------------------
-- The window
----------------------------------------------------------------------------------------------------

function ItemFinder:SetStatus(text)
    if self.status then
        self.status:SetText(text or "")
    end
end

function ItemFinder:AddItem(entry, count)
    count = tonumber(count) or 1
    if count < 1 then
        count = 1
    end
    if count > 1000 then
        count = 1000
    end

    -- The server reads a leading dot off an ordinary say as a command, so this never reaches
    -- anybody's chat. Whether it succeeds is the server's to report: a full bag or a missing GM
    -- rank comes back as a system message in the chat frame, which is left alone on purpose so
    -- that failures are visible rather than swallowed by this window.
    SendChatMessage(".additem " .. entry.id .. " " .. count, "SAY")

    local coloured = entry.name
    if entry.colour then
        coloured = "|c" .. entry.colour .. entry.name .. "|r"
    end

    self:SetStatus("Sent: " .. count .. " x " .. coloured .. "  (id " .. entry.id .. ")")
end

function ItemFinder:Refresh()
    local total = table.getn(self.results)
    local maxOffset = total - ROWS
    if maxOffset < 0 then
        maxOffset = 0
    end
    if self.offset > maxOffset then
        self.offset = maxOffset
    end

    self.slider:SetMinMaxValues(0, maxOffset)
    self.slider:SetValue(self.offset)
    if maxOffset > 0 then
        self.slider:Show()
    else
        self.slider:Hide()
    end

    for n = 1, ROWS do
        local row = self.rows[n]
        local entry = self.results[self.offset + n]

        if not entry then
            row:Hide()
        else
            row.itemId = entry.id
            row.entry = entry

            local icon, stack = ItemIconAndStack(entry.id)
            row.icon:SetNormalTexture(icon or PLACEHOLDER_ICON)
            row.hasIcon = (icon ~= nil)
            row.maxStack = stack

            local br, bg, bb = ColourToRGB(entry.colour)
            row.border:SetBackdropBorderColor(br, bg, bb, 1)

            if entry.colour then
                row.name:SetText("|c" .. entry.colour .. entry.name .. "|r")
            else
                row.name:SetText(entry.name)
            end

            row.id:SetText(entry.id)
            row.count:SetText(entry.count)
            row:Show()
        end
    end

    -- Everything in view and a page beyond it, so the data is on its way before it is needed.
    local last = self.offset + ROWS + ICON_PRIME_AHEAD
    if last > total then
        last = total
    end

    for n = self.offset + 1, last do
        local entry = self.results[n]
        if entry then
            PrimeItem(entry.id)
        end
    end

    if total == 0 then
        if self.query == "" then
            self:SetStatus("Type part of an item name and press Enter.")
        else
            self:SetStatus("Nothing matches \"" .. self.query .. "\".")
        end
    elseif self.resultCount > total then
        self:SetStatus(total .. " of " .. self.resultCount .. " matches for \"" .. self.query ..
                       "\" - narrow the search to see the rest.")
    else
        self:SetStatus(total .. " match(es) for \"" .. self.query .. "\".")
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
    self.offset = 0
    self.capturing = true
    self.deadline = GetTime() + QUIET_TIMEOUT
    primed = {}

    for n = 1, ROWS do
        self.rows[n]:Hide()
    end

    self:SetStatus("Searching for \"" .. text .. "\" ...")
    SendChatMessage(".lookup item " .. text, "SAY")
end

-- Anchored the way a bag slot anchors: the tooltip's bottom-right corner sits on the icon's
-- top-left, so it opens up and to the left and never lands under the cursor or over the row's own
-- count box and button. ANCHOR_NONE first, because SetOwner with any other anchor would place it
-- itself and the explicit point below would be fighting that.
local function Icon_ShowTooltip()
    local row = this:GetParent()
    if not row.itemId then
        return
    end

    GameTooltip:SetOwner(this, "ANCHOR_NONE")
    GameTooltip:ClearAllPoints()
    GameTooltip:SetPoint("BOTTOMRIGHT", this, "TOPLEFT", 0, 0)
    GameTooltip:SetHyperlink(ItemLink(row.itemId))
    GameTooltip:Show()
end

local function Icon_HideTooltip()
    GameTooltip:Hide()
end

local function BuildRow(parent, index)
    local row = CreateFrame("Button", "ItemFinderRow" .. index, parent)
    row:SetWidth(452)
    row:SetHeight(ROW_HEIGHT)
    row:SetPoint("TOPLEFT", parent, "TOPLEFT", 0, -((index - 1) * ROW_HEIGHT))

    local stripe = row:CreateTexture(nil, "BACKGROUND")
    stripe:SetAllPoints(row)
    stripe:SetTexture(1, 1, 1, (math.mod(index, 2) == 0) and 0.03 or 0.06)

    local icon = CreateFrame("Button", "ItemFinderRow" .. index .. "Icon", row)
    icon:SetWidth(30)
    icon:SetHeight(30)
    icon:SetPoint("LEFT", row, "LEFT", 4, 0)
    icon:SetNormalTexture(PLACEHOLDER_ICON)
    icon:GetNormalTexture():SetTexCoord(0.07, 0.93, 0.07, 0.93)
    -- A slot frame around the art, tinted by the item's quality.
    --
    -- The same border art the list itself uses, so the two read as one window rather than as an
    -- icon that happens to have a box round it. Drawn a level above the icon so the edge sits on
    -- top of the texture instead of behind it, and left without mouse handling so it cannot come
    -- between the cursor and the button underneath.
    local border = CreateFrame("Frame", nil, row)
    border:SetPoint("TOPLEFT", icon, "TOPLEFT", -4, 4)
    border:SetPoint("BOTTOMRIGHT", icon, "BOTTOMRIGHT", 4, -4)
    border:SetBackdrop({
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        edgeSize = 12,
    })
    border:SetFrameLevel(icon:GetFrameLevel() + 1)
    border:SetBackdropBorderColor(0.45, 0.45, 0.45, 1)

    icon:SetScript("OnEnter", Icon_ShowTooltip)
    icon:SetScript("OnLeave", Icon_HideTooltip)
    icon:SetScript("OnClick", function()
        ItemFinder:AddItem(this:GetParent().entry, this:GetParent().count:GetText())
    end)

    local name = row:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    name:SetPoint("LEFT", icon, "RIGHT", 8, 0)
    name:SetWidth(228)
    name:SetJustifyH("LEFT")

    local id = row:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    id:SetPoint("LEFT", name, "RIGHT", 4, 0)
    id:SetWidth(52)
    id:SetJustifyH("RIGHT")

    local count = CreateFrame("EditBox", "ItemFinderRow" .. index .. "Count", row, "InputBoxTemplate")
    count:SetPoint("LEFT", id, "RIGHT", 12, 0)
    count:SetWidth(34)
    count:SetHeight(18)
    count:SetAutoFocus(false)
    count:SetNumeric(true)
    count:SetMaxLetters(4)
    count:SetText("1")
    count:SetScript("OnTextChanged", function()
        local parent = this:GetParent()
        if parent.entry then
            parent.entry.count = tonumber(this:GetText()) or 1
        end
    end)
    count:SetScript("OnEnterPressed", function()
        local parent = this:GetParent()
        ItemFinder:AddItem(parent.entry, this:GetText())
        this:ClearFocus()
    end)
    count:SetScript("OnEscapePressed", function()
        this:ClearFocus()
    end)

    local add = CreateFrame("Button", nil, row, "UIPanelButtonTemplate")
    add:SetPoint("LEFT", count, "RIGHT", 8, 0)
    add:SetWidth(60)
    add:SetHeight(21)
    add:SetText("Add")
    add:SetScript("OnClick", function()
        local parent = this:GetParent()

        -- Shift-click means a full stack, which is the count almost every bulk request actually
        -- wants and is otherwise a number the person has to go and look up.
        if IsShiftKeyDown() and parent.maxStack then
            parent.count:SetText(parent.maxStack)
        end

        ItemFinder:AddItem(parent.entry, parent.count:GetText())
    end)

    row.icon = icon
    row.border = border
    row.name = name
    row.id = id
    row.count = count
    row.add = add
    row:Hide()

    return row
end

local function BuildWindow()
    local frame = CreateFrame("Frame", "ItemFinderFrame", UIParent)
    frame:SetWidth(500)
    frame:SetHeight(460)
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

    local input = CreateFrame("EditBox", "ItemFinderInput", frame, "InputBoxTemplate")
    input:SetPoint("TOPLEFT", frame, "TOPLEFT", 26, -44)
    input:SetWidth(320)
    input:SetHeight(20)
    input:SetAutoFocus(false)
    input:SetScript("OnEnterPressed", function()
        ItemFinder:Search(this:GetText())
    end)
    input:SetScript("OnEscapePressed", function()
        this:ClearFocus()
    end)

    local search = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    search:SetPoint("LEFT", input, "RIGHT", 12, 0)
    search:SetWidth(90)
    search:SetHeight(22)
    search:SetText("Search")
    search:SetScript("OnClick", function()
        ItemFinder:Search(ItemFinderInput:GetText())
    end)

    -- The list, and a slider of its own rather than a scroll frame.
    --
    -- Eight rows are built once and re-pointed at different results as the list scrolls, which is
    -- both cheaper than a frame per match and the only way three hundred results stay responsive on
    -- this client. The slider is hand-rolled because the FauxScrollFrame helpers changed signature
    -- between this client and later ones, and getting that wrong is an empty list with no error.
    local list = CreateFrame("Frame", "ItemFinderList", frame)
    list:SetPoint("TOPLEFT", frame, "TOPLEFT", 16, -76)
    list:SetWidth(452)
    list:SetHeight(ROWS * ROW_HEIGHT)
    list:EnableMouseWheel(true)
    list:SetScript("OnMouseWheel", function()
        ItemFinder.offset = ItemFinder.offset - arg1
        if ItemFinder.offset < 0 then
            ItemFinder.offset = 0
        end
        ItemFinder:Refresh()
    end)

    local border = CreateFrame("Frame", nil, frame)
    border:SetPoint("TOPLEFT", list, "TOPLEFT", -6, 6)
    border:SetPoint("BOTTOMRIGHT", list, "BOTTOMRIGHT", 6, -6)
    border:SetBackdrop({
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        edgeSize = 14,
        insets = { left = 3, right = 3, top = 3, bottom = 3 },
    })

    local slider = CreateFrame("Slider", "ItemFinderSlider", frame)
    slider:SetPoint("TOPRIGHT", list, "TOPRIGHT", 20, 0)
    slider:SetWidth(16)
    slider:SetHeight(ROWS * ROW_HEIGHT)
    slider:SetOrientation("VERTICAL")
    slider:SetThumbTexture("Interface\\Buttons\\UI-SliderBar-Button-Vertical")
    slider:SetBackdrop({
        bgFile = "Interface\\Buttons\\UI-SliderBar-Background",
        edgeFile = "Interface\\Buttons\\UI-SliderBar-Border",
        tile = true, tileSize = 8, edgeSize = 8,
        insets = { left = 3, right = 3, top = 6, bottom = 6 },
    })
    slider:SetValueStep(1)
    slider:SetMinMaxValues(0, 0)
    slider:SetValue(0)
    slider:SetScript("OnValueChanged", function()
        local value = this:GetValue()
        if value ~= ItemFinder.offset then
            ItemFinder.offset = value
            ItemFinder:Refresh()
        end
    end)
    slider:Hide()

    for n = 1, ROWS do
        ItemFinder.rows[n] = BuildRow(list, n)
    end

    local status = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    status:SetPoint("TOPLEFT", list, "BOTTOMLEFT", 0, -16)
    status:SetWidth(452)
    status:SetJustifyH("LEFT")
    status:SetText("Type part of an item name and press Enter.")

    local hint = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    hint:SetPoint("TOPLEFT", status, "BOTTOMLEFT", 0, -8)
    hint:SetWidth(452)
    hint:SetJustifyH("LEFT")
    hint:SetText("Hover the icon for the tooltip. Click the icon or Add to deliver. Shift-click " ..
             "Add for a full stack. The server reports failures in chat.")

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
    ItemFinder.list = list
    ItemFinder.slider = slider
end

----------------------------------------------------------------------------------------------------
-- Wiring
----------------------------------------------------------------------------------------------------

BuildWindow()

local ticker = CreateFrame("Frame")
ticker:SetScript("OnUpdate", function()
    local now = GetTime()

    if ItemFinder.capturing and now > ItemFinder.deadline then
        ItemFinder:Finish()
        return
    end

    -- Icons for items the client had never seen arrive a moment after the server is asked for them,
    -- and nothing tells the addon when. Re-checking briefly is cheaper than a per-item query and
    -- stops the list sitting on question marks for items that do have art.
    if now < ItemFinder.iconRetryUntil and now > ItemFinder.nextIconCheck then
        ItemFinder.nextIconCheck = now + ICON_RETRY_INTERVAL

        local missing = false
        for n = 1, ROWS do
            local row = ItemFinder.rows[n]
            if row:IsVisible() and not row.hasIcon then
                missing = true
            end
        end

        if missing then
            ItemFinder:Refresh()
        else
            ItemFinder.iconRetryUntil = 0
        end
    end
end)

-- Swallow the search reply before chat prints it.
--
-- The 1.12 client has no message filter API, so the only place to stand between the event and the
-- chat frame is ChatFrame_OnEvent itself. Only lines that parse as results of a search this addon
-- started are held back: anything else, including every system message while no search is running
-- and every reply to an .additem, goes through untouched, which is what makes a failed delivery
-- visible in chat rather than lost in here.
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
