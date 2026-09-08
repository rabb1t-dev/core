-- PartyBuilder
--
-- Pick four companions by class, role, spec and level, then send the party out in one click. A
-- front end for ".partybot add", which already takes all four of those as arguments; this exists so
-- choosing them is a matter of looking at a party rather than remembering an argument order.
--
-- Same shape as ItemFinder deliberately: the dialog background and tooltip-border art, a bordered
-- icon per row with the border tinted to mean something, a status line that reports what was sent,
-- and the server's own replies left in the chat frame so failures stay visible.
--
-- Written for the 1.12 client, so: Lua 5.0 (string.find with captures, no string.match, table.getn
-- rather than #), event handlers read the globals "this" and "arg1", and no dropdown or scroll
-- helper from FrameXML is used - their signatures moved after this client and a wrong guess is a
-- dead control with no error to explain it. The picker below is four buttons and a popup.

local SLOTS = 4
local ROW_HEIGHT = 40

----------------------------------------------------------------------------------------------------
-- Static data
----------------------------------------------------------------------------------------------------

-- Which jobs each class can actually be asked for. Taken from the server's own answer to that
-- question: GetDefaultSpecNameForRole has a talent build for exactly these pairings and refuses the
-- rest, so a role missing here is one ".partybot add" would reject.
local CLASSES = {
    { id = 1,  key = "warrior", name = "Warrior", colour = "C79C6E",
      roles = { "tank", "melee" } },
    { id = 2,  key = "paladin", name = "Paladin", colour = "F58CBA", team = "Alliance",
      roles = { "tank", "healer", "melee" } },
    { id = 3,  key = "hunter",  name = "Hunter",  colour = "ABD473",
      roles = { "ranged" } },
    { id = 4,  key = "rogue",   name = "Rogue",   colour = "FFF569",
      roles = { "melee" } },
    { id = 5,  key = "priest",  name = "Priest",  colour = "FFFFFF",
      roles = { "healer", "ranged" } },
    { id = 7,  key = "shaman",  name = "Shaman",  colour = "0070DE", team = "Horde",
      roles = { "healer", "melee", "ranged" } },
    { id = 8,  key = "mage",    name = "Mage",    colour = "69CCF0",
      roles = { "ranged" } },
    { id = 9,  key = "warlock", name = "Warlock", colour = "9482C9",
      roles = { "ranged" } },
    { id = 11, key = "druid",   name = "Druid",   colour = "FF7D0A",
      roles = { "tank", "healer", "melee", "ranged" } },
}

local ROLE_LABELS = {
    tank   = "Tank",
    healer = "Healer",
    melee  = "Melee DPS",
    ranged = "Ranged DPS",
}

-- Class icons come out of the target frame's own sheet, which is a fixed four-by-four grid and has
-- been at this path since this client shipped. The per-class icon files are less certain to exist.
local CLASS_ICON_TEXTURE = "Interface\\TargetingFrame\\UI-Classes-Circles"
local CLASS_ICON_COORDS = {
    [1]  = { 0,           0.25,       0,    0.25 },
    [8]  = { 0.25,        0.49609375, 0,    0.25 },
    [4]  = { 0.49609375,  0.7421875,  0,    0.25 },
    [11] = { 0.7421875,   0.98828125, 0,    0.25 },
    [3]  = { 0,           0.25,       0.25, 0.5  },
    [7]  = { 0.25,        0.49609375, 0.25, 0.5  },
    [5]  = { 0.49609375,  0.7421875,  0.25, 0.5  },
    [9]  = { 0.7421875,   0.98828125, 0.25, 0.5  },
    [2]  = { 0,           0.25,       0.5,  0.75 },
}

-- Generated from player_premade_spell_template: the talent builds the server actually has.
-- A spec offered here is one that produces talents; anything else falls back to whatever the
-- level bracket provides, which is not what picking a spec is meant to mean.
local SPECS = {
    [1] = {
        { level = 19, name = "arms-fury-19-twink" },
        { level = 29, name = "arms-29-twink" },
        { level = 39, name = "arms-39-twink" },
        { level = 49, name = "arms-49-twink" },
        { level = 60, name = "arms-pve" },
        { level = 60, name = "fury-dw-pve" },
        { level = 60, name = "protection-pve" },
    },
    [2] = {
        { level = 19, name = "prot-ret-19-twink" },
        { level = 29, name = "retribution-29-twink" },
        { level = 39, name = "retribution-39-twink" },
        { level = 49, name = "retribution-49-twink" },
        { level = 60, name = "holy-pve" },
        { level = 60, name = "protection-pve" },
        { level = 60, name = "retribution-pve" },
    },
    [3] = {
        { level = 19, name = "mm-survival-19-twink" },
        { level = 29, name = "mm-29-twink" },
        { level = 39, name = "mm-39-twink" },
        { level = 49, name = "mm-49-twink" },
        { level = 60, name = "mm-sv-pve" },
    },
    [4] = {
        { level = 19, name = "daggers-19-twink" },
        { level = 29, name = "assa-daggers-29-twink" },
        { level = 39, name = "assa-daggers-39-twink" },
        { level = 49, name = "assa-daggers-49-twink" },
        { level = 60, name = "combat-swords-pve" },
        { level = 60, name = "seal-fate-daggers-pve" },
    },
    [5] = {
        { level = 19, name = "holy-shadow-19-twink" },
        { level = 29, name = "shadow-29-twink" },
        { level = 39, name = "shadow-39-twink" },
        { level = 49, name = "shadow-49-twink" },
        { level = 60, name = "discipline-holy-pve" },
        { level = 60, name = "holy-pve" },
        { level = 60, name = "shadow-pve" },
    },
    [7] = {
        { level = 19, name = "ele-enha-19-twink" },
        { level = 19, name = "resto-19" },
        { level = 29, name = "enha-29-twink" },
        { level = 39, name = "enha-39-twink" },
        { level = 49, name = "enha-49-twink" },
        { level = 60, name = "elemental-pve" },
        { level = 60, name = "enha-resto-pve" },
        { level = 60, name = "resto-pve" },
    },
    [8] = {
        { level = 19, name = "frost-19-twink" },
        { level = 29, name = "frost-29-twink" },
        { level = 39, name = "frost-39-twink" },
        { level = 49, name = "frost-49-twink" },
        { level = 60, name = "arcane-power-frost-pve" },
        { level = 60, name = "fire-pve" },
    },
    [9] = {
        { level = 19, name = "affli-demo-19-twink" },
        { level = 29, name = "affliction-29-twink" },
        { level = 39, name = "affliction-39-twink" },
        { level = 49, name = "affli-sb-49-twink" },
        { level = 60, name = "ds-ruin-pve" },
        { level = 60, name = "sm-ruin-pve" },
    },
    [11] = {
        { level = 19, name = "balance-fc-19-twink" },
        { level = 29, name = "feral-fc-29-twink" },
        { level = 39, name = "feral-fc-39-twink" },
        { level = 49, name = "feral-fc-49-twink" },
        { level = 60, name = "balance-pve" },
        { level = 60, name = "feral-bear-pve" },
        { level = 60, name = "feral-cat-pve" },
        { level = 60, name = "resto-swiftmend-pve" },
    },
}

-- A ready-made party for when the point is to go somewhere rather than to fiddle with a roster.
local PRESET = {
    { class = "warrior", role = "tank" },
    { class = "priest",  role = "healer" },
    { class = "rogue",   role = "melee" },
    { class = "mage",    role = "ranged" },
}

----------------------------------------------------------------------------------------------------
-- State
----------------------------------------------------------------------------------------------------

local PartyBuilder = {}
PartyBuilder.slots = {}
PartyBuilder.rows = {}

for n = 1, SLOTS do
    PartyBuilder.slots[n] = { classId = nil, role = nil, spec = nil, level = nil }
end

local function FindClass(id)
    for n = 1, table.getn(CLASSES) do
        if CLASSES[n].id == id then
            return CLASSES[n]
        end
    end
    return nil
end

local function FindClassByKey(key)
    for n = 1, table.getn(CLASSES) do
        if CLASSES[n].key == key then
            return CLASSES[n]
        end
    end
    return nil
end

local function MyTeam()
    return UnitFactionGroup("player")
end

-- Only the classes this faction has. A Horde player offered Paladin gets a command the server
-- refuses, which is a worse answer than not offering it.
local function AvailableClasses()
    local team = MyTeam()
    local out = {}

    for n = 1, table.getn(CLASSES) do
        local class = CLASSES[n]
        if not class.team or class.team == team then
            table.insert(out, class)
        end
    end

    return out
end

local function EffectiveLevel(slot)
    return slot.level or UnitLevel("player")
end

-- The talent bracket a level falls in. Templates exist at 19, 29, 39, 49 and 60, and the server
-- takes the highest one at or below the character's level, so a level 25 bot is a 19 build. Showing
-- specs from any other bracket would be offering builds that will not be used.
local function BracketFor(level)
    local brackets = { 60, 49, 39, 29, 19 }

    for n = 1, table.getn(brackets) do
        if level >= brackets[n] then
            return brackets[n]
        end
    end

    return nil
end

local function SpecsFor(slot)
    local out = {}

    if not slot.classId then
        return out
    end

    local bracket = BracketFor(EffectiveLevel(slot))
    if not bracket then
        return out
    end

    local list = SPECS[slot.classId]
    if not list then
        return out
    end

    for n = 1, table.getn(list) do
        if list[n].level == bracket then
            table.insert(out, list[n].name)
        end
    end

    return out
end

local function Composition()
    local tanks, healers, dps = 0, 0, 0

    for n = 1, SLOTS do
        local slot = PartyBuilder.slots[n]
        if slot.classId and slot.role then
            if slot.role == "tank" then
                tanks = tanks + 1
            elseif slot.role == "healer" then
                healers = healers + 1
            else
                dps = dps + 1
            end
        end
    end

    return tanks, healers, dps
end

----------------------------------------------------------------------------------------------------
-- The shared picker popup
----------------------------------------------------------------------------------------------------

local picker = nil
local PICKER_ROWS = 12

local function HidePicker()
    if picker then
        picker:Hide()
    end
end

local function BuildPicker()
    local frame = CreateFrame("Frame", "PartyBuilderPicker", UIParent)
    frame:SetWidth(190)
    frame:SetFrameStrata("DIALOG")
    frame:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 14,
        insets = { left = 4, right = 4, top = 4, bottom = 4 },
    })
    frame:EnableMouse(true)
    frame:Hide()

    frame.buttons = {}

    for n = 1, PICKER_ROWS do
        local button = CreateFrame("Button", nil, frame)
        button:SetWidth(174)
        button:SetHeight(17)
        button:SetPoint("TOPLEFT", frame, "TOPLEFT", 8, -(6 + (n - 1) * 17))

        local highlight = button:CreateTexture(nil, "HIGHLIGHT")
        highlight:SetAllPoints(button)
        highlight:SetTexture(1, 1, 1, 0.15)

        local label = button:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        label:SetPoint("LEFT", button, "LEFT", 2, 0)
        label:SetJustifyH("LEFT")
        button.label = label

        button:SetScript("OnClick", function()
            HidePicker()
            if this.action then
                this.action(this.value)
            end
        end)

        button:Hide()
        frame.buttons[n] = button
    end

    -- Clicking anywhere else closes it, which is what a menu is expected to do and is otherwise a
    -- popup that has to be dismissed by choosing something.
    frame:SetScript("OnHide", function()
        for n = 1, PICKER_ROWS do
            frame.buttons[n].action = nil
        end
    end)

    picker = frame
end

-- entries: list of { text = display, value = what the action receives }
local function ShowPicker(anchor, entries, action)
    if not picker then
        BuildPicker()
    end

    local count = table.getn(entries)
    if count > PICKER_ROWS then
        count = PICKER_ROWS
    end

    for n = 1, PICKER_ROWS do
        local button = picker.buttons[n]
        local entry = entries[n]

        if n <= count and entry then
            button.label:SetText(entry.text)
            button.value = entry.value
            button.action = action
            button:Show()
        else
            button:Hide()
        end
    end

    picker:SetHeight(12 + count * 17)
    picker:ClearAllPoints()
    picker:SetPoint("TOPLEFT", anchor, "BOTTOMLEFT", 0, -2)
    picker:Show()
end

----------------------------------------------------------------------------------------------------
-- Rows
----------------------------------------------------------------------------------------------------

local function ColourToRGB(hex)
    local r = tonumber(string.sub(hex, 1, 2), 16)
    local g = tonumber(string.sub(hex, 3, 4), 16)
    local b = tonumber(string.sub(hex, 5, 6), 16)

    if not r or not g or not b then
        return 0.45, 0.45, 0.45
    end

    return r / 255, g / 255, b / 255
end

function PartyBuilder:SetStatus(text)
    if self.status then
        self.status:SetText(text or "")
    end
end

function PartyBuilder:Refresh()
    for n = 1, SLOTS do
        local row = self.rows[n]
        local slot = self.slots[n]
        local class = slot.classId and FindClass(slot.classId) or nil

        if class then
            local coords = CLASS_ICON_COORDS[class.id]
            row.icon:SetNormalTexture(CLASS_ICON_TEXTURE)
            row.icon:GetNormalTexture():SetTexCoord(coords[1], coords[2], coords[3], coords[4])
            row.border:SetBackdropBorderColor(ColourToRGB(class.colour))
            row.classButton:SetText("|cff" .. class.colour .. class.name .. "|r")
        else
            row.icon:SetNormalTexture("Interface\\Icons\\INV_Misc_QuestionMark")
            row.icon:GetNormalTexture():SetTexCoord(0.07, 0.93, 0.07, 0.93)
            row.border:SetBackdropBorderColor(0.35, 0.35, 0.35, 1)
            row.classButton:SetText("Empty")
        end

        if slot.role then
            row.roleButton:SetText(ROLE_LABELS[slot.role] or slot.role)
        else
            row.roleButton:SetText(class and "Pick role" or "-")
        end

        if slot.spec then
            row.specButton:SetText(slot.spec)
        else
            row.specButton:SetText(class and "Default" or "-")
        end

        row.levelBox:SetText(slot.level or "")
    end

    local tanks, healers, dps = Composition()
    local filled = tanks + healers + dps

    self.summary:SetText("Party of " .. (filled + 1) .. ": you plus " .. filled ..
                         "  |cff888888(" .. tanks .. " tank, " .. healers .. " healer, " ..
                         dps .. " dps)|r")
end

function PartyBuilder:PickClass(index)
    local row = self.rows[index]
    local entries = {}
    local available = AvailableClasses()

    table.insert(entries, { text = "|cff888888Empty|r", value = 0 })

    for n = 1, table.getn(available) do
        local class = available[n]
        table.insert(entries, {
            text = "|cff" .. class.colour .. class.name .. "|r",
            value = class.id,
        })
    end

    ShowPicker(row.classButton, entries, function(value)
        local slot = PartyBuilder.slots[index]

        if value == 0 then
            slot.classId = nil
            slot.role = nil
            slot.spec = nil
        else
            slot.classId = value
            -- Role and spec belong to the class that was there before, so they do not survive it.
            -- A shaman keeping "tank" from the warrior it replaced is a command the server refuses.
            local class = FindClass(value)
            slot.role = class.roles[1]
            slot.spec = nil
        end

        PartyBuilder:Refresh()
    end)
end

function PartyBuilder:PickRole(index)
    local slot = self.slots[index]
    if not slot.classId then
        return
    end

    local class = FindClass(slot.classId)
    local entries = {}

    for n = 1, table.getn(class.roles) do
        local role = class.roles[n]
        table.insert(entries, { text = ROLE_LABELS[role] or role, value = role })
    end

    ShowPicker(self.rows[index].roleButton, entries, function(value)
        PartyBuilder.slots[index].role = value
        -- The spec was chosen for the old role and may belong to a different tree entirely.
        PartyBuilder.slots[index].spec = nil
        PartyBuilder:Refresh()
    end)
end

function PartyBuilder:PickSpec(index)
    local slot = self.slots[index]
    if not slot.classId then
        return
    end

    local specs = SpecsFor(slot)
    local entries = { { text = "|cff888888Default for role|r", value = "" } }

    for n = 1, table.getn(specs) do
        table.insert(entries, { text = specs[n], value = specs[n] })
    end

    if table.getn(specs) == 0 then
        entries = { { text = "|cff888888No builds below level 19|r", value = "" } }
    end

    ShowPicker(self.rows[index].specButton, entries, function(value)
        if value == "" then
            PartyBuilder.slots[index].spec = nil
        else
            PartyBuilder.slots[index].spec = value
        end
        PartyBuilder:Refresh()
    end)
end

function PartyBuilder:UsePreset()
    local team = MyTeam()

    for n = 1, SLOTS do
        local slot = self.slots[n]
        local wanted = PRESET[n]
        local class = wanted and FindClassByKey(wanted.class) or nil

        if class and (not class.team or class.team == team) then
            slot.classId = class.id
            slot.role = wanted.role
            slot.spec = nil
        else
            slot.classId = nil
            slot.role = nil
            slot.spec = nil
        end
    end

    self:Refresh()
    self:SetStatus("Loaded a standard party. Adjust anything, then Create.")
end

function PartyBuilder:Clear()
    for n = 1, SLOTS do
        self.slots[n] = { classId = nil, role = nil, spec = nil, level = nil }
    end

    self:Refresh()
    self:SetStatus("Cleared.")
end

function PartyBuilder:Create()
    local sent = 0

    for n = 1, SLOTS do
        local slot = self.slots[n]

        if slot.classId and slot.role then
            local class = FindClass(slot.classId)

            -- Level before spec, because that is the order the command parses them in and a spec
            -- sent where a level is expected is read as a level, fails to be one, and then the spec
            -- has already been consumed.
            local command = ".partybot add " .. class.key .. " " .. slot.role

            if slot.level then
                command = command .. " " .. slot.level
            end

            if slot.spec then
                command = command .. " " .. slot.spec
            end

            SendChatMessage(command, "SAY")
            sent = sent + 1
        end
    end

    if sent == 0 then
        self:SetStatus("Nothing to create - every slot is empty.")
    else
        self:SetStatus("Sent " .. sent .. " companion" .. ((sent == 1) and "" or "s") ..
                       ". The server reports each one in chat.")
    end
end

function PartyBuilder:Disband()
    SendChatMessage(".partybot removeall", "SAY")
    self:SetStatus("Asked the server to dismiss every party bot in the group.")
end

local function BuildRow(parent, index)
    local row = CreateFrame("Frame", "PartyBuilderRow" .. index, parent)
    row:SetWidth(478)
    row:SetHeight(ROW_HEIGHT)
    row:SetPoint("TOPLEFT", parent, "TOPLEFT", 0, -((index - 1) * ROW_HEIGHT))

    local stripe = row:CreateTexture(nil, "BACKGROUND")
    stripe:SetAllPoints(row)
    stripe:SetTexture(1, 1, 1, (math.mod(index, 2) == 0) and 0.03 or 0.06)

    local icon = CreateFrame("Button", nil, row)
    icon:SetWidth(28)
    icon:SetHeight(28)
    icon:SetPoint("LEFT", row, "LEFT", 6, 0)
    icon:SetNormalTexture("Interface\\Icons\\INV_Misc_QuestionMark")
    icon:EnableMouse(false)

    -- The same bordered icon as ItemFinder's rows, tinted by class rather than by quality, so the
    -- party reads as a party at a glance instead of as four lines of text.
    local border = CreateFrame("Frame", nil, row)
    border:SetPoint("TOPLEFT", icon, "TOPLEFT", -4, 4)
    border:SetPoint("BOTTOMRIGHT", icon, "BOTTOMRIGHT", 4, -4)
    border:SetBackdrop({
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        edgeSize = 12,
    })
    border:SetFrameLevel(icon:GetFrameLevel() + 1)
    border:SetBackdropBorderColor(0.35, 0.35, 0.35, 1)

    local classButton = CreateFrame("Button", nil, row, "UIPanelButtonTemplate")
    classButton:SetPoint("LEFT", icon, "RIGHT", 10, 0)
    classButton:SetWidth(92)
    classButton:SetHeight(21)
    classButton:SetScript("OnClick", function()
        PartyBuilder:PickClass(index)
    end)

    local roleButton = CreateFrame("Button", nil, row, "UIPanelButtonTemplate")
    roleButton:SetPoint("LEFT", classButton, "RIGHT", 6, 0)
    roleButton:SetWidth(86)
    roleButton:SetHeight(21)
    roleButton:SetScript("OnClick", function()
        PartyBuilder:PickRole(index)
    end)

    local specButton = CreateFrame("Button", nil, row, "UIPanelButtonTemplate")
    specButton:SetPoint("LEFT", roleButton, "RIGHT", 6, 0)
    specButton:SetWidth(150)
    specButton:SetHeight(21)
    specButton:SetScript("OnClick", function()
        PartyBuilder:PickSpec(index)
    end)

    local levelBox = CreateFrame("EditBox", "PartyBuilderLevel" .. index, row, "InputBoxTemplate")
    levelBox:SetPoint("LEFT", specButton, "RIGHT", 14, 0)
    levelBox:SetWidth(32)
    levelBox:SetHeight(18)
    levelBox:SetAutoFocus(false)
    levelBox:SetNumeric(true)
    levelBox:SetMaxLetters(2)
    levelBox:SetScript("OnTextChanged", function()
        local value = tonumber(this:GetText())

        -- Blank means "same as me", which is what the command does with no level at all. Storing
        -- the player's level instead would freeze it, and the number would then be wrong the next
        -- time the window is opened after a level up.
        if value and value >= 1 and value <= 60 then
            PartyBuilder.slots[index].level = value
        else
            PartyBuilder.slots[index].level = nil
        end

        -- The spec list is chosen by level bracket, so a spec picked for one level can stop being
        -- available at another.
        local specs = SpecsFor(PartyBuilder.slots[index])
        local slotSpec = PartyBuilder.slots[index].spec
        if slotSpec then
            local stillThere = false
            for n = 1, table.getn(specs) do
                if specs[n] == slotSpec then
                    stillThere = true
                end
            end
            if not stillThere then
                PartyBuilder.slots[index].spec = nil
            end
        end

        PartyBuilder:Refresh()
    end)
    levelBox:SetScript("OnEscapePressed", function()
        this:ClearFocus()
    end)

    row.icon = icon
    row.border = border
    row.classButton = classButton
    row.roleButton = roleButton
    row.specButton = specButton
    row.levelBox = levelBox

    return row
end

local function BuildWindow()
    local frame = CreateFrame("Frame", "PartyBuilderFrame", UIParent)
    frame:SetWidth(520)
    frame:SetHeight(360)
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
    frame:SetScript("OnMouseDown", HidePicker)
    frame:SetScript("OnHide", HidePicker)
    frame:Hide()

    table.insert(UISpecialFrames, "PartyBuilderFrame")

    local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    title:SetPoint("TOP", frame, "TOP", 0, -14)
    title:SetText("PartyBuilder")

    local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -4, -4)

    local summary = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    summary:SetPoint("TOPLEFT", frame, "TOPLEFT", 22, -40)
    summary:SetWidth(478)
    summary:SetJustifyH("LEFT")

    local header = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    header:SetPoint("TOPLEFT", frame, "TOPLEFT", 66, -58)
    header:SetJustifyH("LEFT")
    header:SetText("Class            Role              Talent build                        Lvl")

    local list = CreateFrame("Frame", "PartyBuilderList", frame)
    list:SetPoint("TOPLEFT", frame, "TOPLEFT", 22, -74)
    list:SetWidth(478)
    list:SetHeight(SLOTS * ROW_HEIGHT)

    local border = CreateFrame("Frame", nil, frame)
    border:SetPoint("TOPLEFT", list, "TOPLEFT", -6, 6)
    border:SetPoint("BOTTOMRIGHT", list, "BOTTOMRIGHT", 6, -6)
    border:SetBackdrop({
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        edgeSize = 14,
        insets = { left = 3, right = 3, top = 3, bottom = 3 },
    })

    for n = 1, SLOTS do
        PartyBuilder.rows[n] = BuildRow(list, n)
    end

    local status = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    status:SetPoint("TOPLEFT", list, "BOTTOMLEFT", 0, -16)
    status:SetWidth(478)
    status:SetJustifyH("LEFT")

    local hint = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    hint:SetPoint("TOPLEFT", status, "BOTTOMLEFT", 0, -6)
    hint:SetWidth(478)
    hint:SetJustifyH("LEFT")
    hint:SetText("Leave Lvl blank to match your own level. Builds are listed for the bracket the " ..
                 "level falls in.")

    local create = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    create:SetPoint("BOTTOMLEFT", frame, "BOTTOMLEFT", 22, 16)
    create:SetWidth(110)
    create:SetHeight(22)
    create:SetText("Create Party")
    create:SetScript("OnClick", function()
        PartyBuilder:Create()
    end)

    local preset = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    preset:SetPoint("LEFT", create, "RIGHT", 8, 0)
    preset:SetWidth(90)
    preset:SetHeight(22)
    preset:SetText("Standard")
    preset:SetScript("OnClick", function()
        PartyBuilder:UsePreset()
    end)

    local clear = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    clear:SetPoint("LEFT", preset, "RIGHT", 8, 0)
    clear:SetWidth(70)
    clear:SetHeight(22)
    clear:SetText("Clear")
    clear:SetScript("OnClick", function()
        PartyBuilder:Clear()
    end)

    local disband = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    disband:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -22, 16)
    disband:SetWidth(90)
    disband:SetHeight(22)
    disband:SetText("Disband")
    disband:SetScript("OnClick", function()
        PartyBuilder:Disband()
    end)

    PartyBuilder.frame = frame
    PartyBuilder.status = status
    PartyBuilder.summary = summary
end

----------------------------------------------------------------------------------------------------
-- Wiring
----------------------------------------------------------------------------------------------------

BuildWindow()
PartyBuilder:UsePreset()
PartyBuilder:SetStatus("Pick four companions, then Create Party.")

SLASH_PARTYBUILDER1 = "/partybuilder"
SLASH_PARTYBUILDER2 = "/pb"
SlashCmdList["PARTYBUILDER"] = function()
    if PartyBuilder.frame:IsVisible() then
        PartyBuilder.frame:Hide()
    else
        PartyBuilder:Refresh()
        PartyBuilder.frame:Show()
    end
end
