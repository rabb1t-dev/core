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

-- Column geometry, written once and used by both the controls and the labels above them.
--
-- The labels used to be anchored to the buttons and nudged upwards by a hand-picked number of
-- pixels, which put them in the window's top border where they were clipped. Anchoring both to the
-- same numbers instead means a column and its title cannot disagree, and the header sits in a row
-- of its own with room for it rather than in whatever space happened to be left.
local COL_CHECK  = 6
local COL_ICON   = 34
local COL_CLASS  = 68
local COL_ROLE   = 166
local COL_SPEC   = 258
local COL_LEVEL  = 384

local W_CLASS = 92
local W_ROLE  = 86
local W_SPEC  = 118
local W_LEVEL = 32

local ROW_WIDTH = COL_LEVEL + W_LEVEL + 10
local HEADER_HEIGHT = 16

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

-- One signature ability per class, rather than a class crest.
--
-- The first attempt cut squares out of the target frame's UI-Classes-Circles sheet, on the
-- reasoning that a four-by-four grid at a known path is safer than nine separate files. It rendered
-- as coloured corners: the sheet's layout on this client is not the one those coordinates assume,
-- and a texture coordinate that is merely wrong produces a picture rather than an error, so nothing
-- said so. These are ordinary spell icons, addressed by name, each unmistakably its class.
local CLASS_ICONS = {
    [1]  = "Interface\\Icons\\Ability_Warrior_OffensiveStance",
    [2]  = "Interface\\Icons\\Spell_Holy_AuraOfLight",
    [3]  = "Interface\\Icons\\Ability_Marksmanship",
    [4]  = "Interface\\Icons\\Ability_BackStab",
    [5]  = "Interface\\Icons\\Spell_Holy_PowerWordShield",
    [7]  = "Interface\\Icons\\Spell_Nature_Lightning",
    [8]  = "Interface\\Icons\\Spell_Frost_FrostBolt02",
    [9]  = "Interface\\Icons\\Spell_Shadow_ShadowBolt",
    [11] = "Interface\\Icons\\Spell_Nature_HealingTouch",
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

-- A ready-made party for when the point is to go somewhere rather than to fiddle with a roster:
-- one tank, one healer, one of each kind of damage.
--
-- Each role lists its classes in preference order and the first one that fits is taken, skipping
-- the player's own class. A mage handed a party containing a mage has been given three companions
-- and a duplicate, and the class it wants instead is already the next name on the list.
local PRESET = {
    { role = "tank",   classes = { "warrior", "druid", "paladin" } },
    { role = "healer", classes = { "priest", "shaman", "druid", "paladin" } },
    { role = "melee",  classes = { "rogue", "warrior", "druid", "shaman" } },
    { role = "ranged", classes = { "mage", "hunter", "warlock", "priest", "druid" } },
}

----------------------------------------------------------------------------------------------------
-- State
----------------------------------------------------------------------------------------------------

local PartyBuilder = {}
PartyBuilder.slots = {}
PartyBuilder.rows = {}

for n = 1, SLOTS do
    PartyBuilder.slots[n] = { enabled = true, classId = nil, role = nil, spec = nil, level = nil }
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

        -- Read the choice before closing the popup, not after.
        --
        -- Hiding it runs its OnHide, which clears every button's action so a stale one cannot fire
        -- against a later menu. Calling HidePicker first therefore threw away the action on the
        -- very button that had just been clicked, and the click did nothing at all - visibly so
        -- when switching a slot from one class to another, which is a change with no other effect
        -- to see.
        button:SetScript("OnClick", function()
            local action = this.action
            local value = this.value

            HidePicker()

            if action then
                action(value)
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

        local on = slot.enabled and class ~= nil

        -- 1 or nil rather than true or false: this client's SetChecked predates booleans being
        -- accepted everywhere, and a false here is the kind of argument it complains about.
        row.check:SetChecked(slot.enabled and 1 or nil)

        if class then
            row.icon:SetNormalTexture(CLASS_ICONS[class.id])
        else
            row.icon:SetNormalTexture("Interface\\Icons\\INV_Misc_QuestionMark")
        end

        row.icon:GetNormalTexture():SetTexCoord(0.07, 0.93, 0.07, 0.93)

        -- Off reads as off at a glance: the art dims, the border loses its class colour, and every
        -- control including the class button greys out. The class button used to stay live on the
        -- reasoning that an off row is still worth configuring, but a row where three controls are
        -- grey and one is not does not look switched off - and nothing is lost, because ticking a
        -- row back on opens the class picker anyway.
        --
        -- The class name also drops its colour code. A coloured font string wins over a disabled
        -- button's own greying, so leaving it in place is a row that looks live while refusing
        -- clicks.
        if on then
            row.icon:GetNormalTexture():SetVertexColor(1, 1, 1)
            row.border:SetBackdropBorderColor(ColourToRGB(class.colour))
            row.classButton:SetText("|cff" .. class.colour .. class.name .. "|r")
            row.classButton:Enable()
            row.roleButton:Enable()
            row.specButton:Enable()
            row.levelBox:EnableMouse(true)
            row.levelBox:SetTextColor(1, 1, 1)
        else
            row.icon:GetNormalTexture():SetVertexColor(0.35, 0.35, 0.35)
            row.border:SetBackdropBorderColor(0.28, 0.28, 0.28, 1)
            row.classButton:SetText(class and class.name or "Pick a class")
            row.classButton:Disable()
            row.roleButton:Disable()
            row.specButton:Disable()
            row.levelBox:EnableMouse(false)
            row.levelBox:SetTextColor(0.45, 0.45, 0.45)
            row.levelBox:ClearFocus()
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

    local wanted = 0
    for n = 1, SLOTS do
        local slot = self.slots[n]
        if slot.enabled and slot.classId then
            wanted = wanted + 1
        end
    end

    self.sizeText:SetText("Party of " .. (wanted + 1))

end

function PartyBuilder:PickClass(index)
    local row = self.rows[index]
    local entries = {}
    local available = AvailableClasses()

    -- No "Empty" here any more. A row that is not wanted is unticked, which is one idea in one
    -- place; having both meant two ways to say the same thing and a row that could be ticked on
    -- while holding no class at all.
    for n = 1, table.getn(available) do
        local class = available[n]
        table.insert(entries, {
            text = "|cff" .. class.colour .. class.name .. "|r",
            value = class.id,
        })
    end

    ShowPicker(row.classButton, entries, function(value)
        local slot = PartyBuilder.slots[index]

        slot.classId = value

        -- Role and spec belong to the class that was there before, so they do not survive it. A
        -- shaman keeping "tank" from the warrior it replaced is a command the server refuses.
        local class = FindClass(value)
        slot.role = class.roles[1]
        slot.spec = nil

        -- Choosing a class for a row is saying you want it.
        slot.enabled = true

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
    local _, myClassKey = UnitClass("player")
    if myClassKey then
        myClassKey = string.lower(myClassKey)
    end

    local taken = {}

    for n = 1, SLOTS do
        local slot = self.slots[n]
        local wanted = PRESET[n]

        slot.classId = nil
        slot.role = nil
        slot.spec = nil
        slot.enabled = false

        if wanted then
            for i = 1, table.getn(wanted.classes) do
                local class = FindClassByKey(wanted.classes[i])

                if class and (not class.team or class.team == team) and
                   class.key ~= myClassKey and not taken[class.key] then
                    slot.classId = class.id
                    slot.role = wanted.role
                    slot.enabled = true
                    taken[class.key] = true
                    break
                end
            end
        end
    end

    self:Refresh()
    self:SetStatus("Standard party: a tank, a healer, and one of each kind of damage.")
end

-- Switches every companion off rather than forgetting what they were, so a party of one is one
-- click away from being a party of five again with the same roster.
function PartyBuilder:Clear()
    for n = 1, SLOTS do
        self.slots[n].enabled = false
    end

    self:Refresh()
    self:SetStatus("All companions off. You will go alone.")
end

function PartyBuilder:Create()
    local sent = 0

    for n = 1, SLOTS do
        local slot = self.slots[n]

        if slot.enabled and slot.classId and slot.role then
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
        self:SetStatus("Nothing to create - no companions are switched on.")
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
    row:SetWidth(ROW_WIDTH)
    row:SetHeight(ROW_HEIGHT)
    row:SetPoint("TOPLEFT", parent, "TOPLEFT", 0, -((index - 1) * ROW_HEIGHT))

    local stripe = row:CreateTexture(nil, "BACKGROUND")
    stripe:SetAllPoints(row)
    stripe:SetTexture(1, 1, 1, (math.mod(index, 2) == 0) and 0.03 or 0.06)

    local check = CreateFrame("CheckButton", "PartyBuilderCheck" .. index, row,
                              "UICheckButtonTemplate")
    check:SetWidth(22)
    check:SetHeight(22)
    check:SetPoint("TOPLEFT", row, "TOPLEFT", COL_CHECK, -6)
    check:SetScript("OnClick", function()
        local slot = PartyBuilder.slots[index]
        slot.enabled = not slot.enabled

        -- Ticking a row that has never been given a class opens the class picker rather than
        -- leaving a row that is on and holds nothing.
        if slot.enabled and not slot.classId then
            PartyBuilder:Refresh()
            PartyBuilder:PickClass(index)
            return
        end

        PartyBuilder:Refresh()
    end)

    local icon = CreateFrame("Button", nil, row)
    icon:SetWidth(28)
    icon:SetHeight(28)
    icon:SetPoint("TOPLEFT", row, "TOPLEFT", COL_ICON, -4)
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
    classButton:SetPoint("TOPLEFT", row, "TOPLEFT", COL_CLASS, -9)
    classButton:SetWidth(W_CLASS)
    classButton:SetHeight(21)
    classButton:SetScript("OnClick", function()
        PartyBuilder:PickClass(index)
    end)

    local roleButton = CreateFrame("Button", nil, row, "UIPanelButtonTemplate")
    roleButton:SetPoint("TOPLEFT", row, "TOPLEFT", COL_ROLE, -9)
    roleButton:SetWidth(W_ROLE)
    roleButton:SetHeight(21)
    roleButton:SetScript("OnClick", function()
        PartyBuilder:PickRole(index)
    end)

    local specButton = CreateFrame("Button", nil, row, "UIPanelButtonTemplate")
    specButton:SetPoint("TOPLEFT", row, "TOPLEFT", COL_SPEC, -9)
    specButton:SetWidth(W_SPEC)
    specButton:SetHeight(21)
    specButton:SetScript("OnClick", function()
        PartyBuilder:PickSpec(index)
    end)

    local levelBox = CreateFrame("EditBox", "PartyBuilderLevel" .. index, row, "InputBoxTemplate")
    levelBox:SetPoint("TOPLEFT", row, "TOPLEFT", COL_LEVEL, -11)
    levelBox:SetWidth(W_LEVEL)
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

    row.check = check
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
    frame:SetWidth(ROW_WIDTH + 44)
    frame:SetHeight(356)
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

    -- Not the sentence that was here before, which spelled out the arithmetic. Four ticks and a
    -- number is the whole of what the window has to say about its own size.
    local sizeText = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    sizeText:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -30, -18)
    sizeText:SetJustifyH("RIGHT")

    local header = CreateFrame("Frame", "PartyBuilderHeader", frame)
    header:SetPoint("TOPLEFT", frame, "TOPLEFT", 22, -44)
    header:SetWidth(ROW_WIDTH)
    header:SetHeight(HEADER_HEIGHT)

    local list = CreateFrame("Frame", "PartyBuilderList", frame)
    list:SetPoint("TOPLEFT", header, "BOTTOMLEFT", 0, -8)
    list:SetWidth(ROW_WIDTH)
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

    -- Column labels anchored to the controls they name rather than laid out as one padded string.
    -- The padded version drifted as soon as a button width changed, which is how "Default" ended up
    -- reading as though it sat under "Lvl".
    local function Header(text, x)
        local label = header:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
        label:SetPoint("LEFT", header, "LEFT", x + 2, 0)
        label:SetJustifyH("LEFT")
        label:SetText(text)
        return label
    end

    Header("Class", COL_CLASS)
    Header("Role", COL_ROLE)
    Header("Talent build", COL_SPEC)

    -- An InputBoxTemplate draws its visible edge a few pixels inside its own frame, so this one
    -- needs that offset back to sit over the box rather than over the gap beside it.
    Header("Lvl", COL_LEVEL + 5)

    local status = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    status:SetPoint("TOPLEFT", list, "BOTTOMLEFT", 0, -16)
    status:SetWidth(ROW_WIDTH)
    status:SetJustifyH("LEFT")

    local hint = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    hint:SetPoint("TOPLEFT", status, "BOTTOMLEFT", 0, -6)
    hint:SetWidth(ROW_WIDTH)
    hint:SetJustifyH("LEFT")
    hint:SetText("Untick a companion to leave them behind. Blank Lvl matches your own level; " ..
                 "builds are listed for the bracket that level falls in.")

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
    PartyBuilder.sizeText = sizeText
end

----------------------------------------------------------------------------------------------------
-- Wiring
----------------------------------------------------------------------------------------------------

BuildWindow()

-- Opens with one companion, not four.
--
-- The roster is still filled in from the preset, so the other three are a tick away and already
-- hold a sensible class and role. But a window that arrives with four switched on has decided what
-- you are doing before you have said: adding is a deliberate act and removing three is a chore, so
-- the default is the smaller of the two. Standard is there for when a full five-man is the answer.
PartyBuilder:UsePreset()

for n = 2, SLOTS do
    PartyBuilder.slots[n].enabled = false
end

PartyBuilder:Refresh()
PartyBuilder:SetStatus("Tick a companion to bring them along, then Create Party.")

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
