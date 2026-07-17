-- DOOM controller for HDZero goggles (EdgeTX tool script)
--
-- Put this file in SCRIPTS/TOOLS/ on the radio SD card and set a free
-- serial port to mode "Lua" (SYS > Hardware > Serial ports). Wire that
-- port's TX pin (plus GND and 5V) to the doom dongle (see doom_dongle.ino).
--
-- While this tool is open, stick positions and switches are encoded as a
-- button mask and streamed out the serial port; the dongle relays them to
-- the goggles over ESP-NOW.
--
-- Default mapping (change the constants below to taste):
--   Elevator      : move forward / back
--   Aileron       : turn left / right
--   Rudder        : strafe left / right
--   FIRE_SW  (SH) : fire
--   USE_SW   (SD) : use / open doors
--   ENTER key     : Enter (Doom menus)
--   EXIT key      : Escape (Doom menu open/close)

local FIRE_SW = "sh"
local USE_SW = "sd"
local STICK_DEADBAND = 256 -- of +/-1024

local BTN_FORWARD = 1
local BTN_BACK = 2
local BTN_TURN_L = 4
local BTN_TURN_R = 8
local BTN_FIRE = 16
local BTN_USE = 32
local BTN_ENTER = 64
local BTN_ESCAPE = 128
local BTN_STRAFE_L = 256
local BTN_STRAFE_R = 512

local lastMask = -1
local lastSendTime = 0
local pulseMask = 0
local pulseUntil = 0

local function sendMask(mask)
  -- frame: '$' 'D' mask_lo mask_hi checksum(xor)
  local lo = mask % 256
  local hi = math.floor(mask / 256)
  local crc = bit32.bxor(lo, hi)
  serialWrite(string.char(0x24, 0x44, lo, hi, crc))
end

local function init()
end

local function run(event)
  local now = getTime() -- 10ms ticks

  -- momentary keys become short pulses in the mask
  if event == EVT_VIRTUAL_ENTER then
    pulseMask = bit32.bor(pulseMask, BTN_ENTER)
    pulseUntil = now + 15
  elseif event == EVT_VIRTUAL_EXIT then
    pulseMask = bit32.bor(pulseMask, BTN_ESCAPE)
    pulseUntil = now + 15
  end
  if pulseUntil ~= 0 and now > pulseUntil then
    pulseMask = 0
    pulseUntil = 0
  end

  local mask = pulseMask
  local ele = getValue("ele")
  local ail = getValue("ail")
  local rud = getValue("rud")

  if ele > STICK_DEADBAND then
    mask = bit32.bor(mask, BTN_FORWARD)
  elseif ele < -STICK_DEADBAND then
    mask = bit32.bor(mask, BTN_BACK)
  end
  if ail > STICK_DEADBAND then
    mask = bit32.bor(mask, BTN_TURN_R)
  elseif ail < -STICK_DEADBAND then
    mask = bit32.bor(mask, BTN_TURN_L)
  end
  if rud > STICK_DEADBAND then
    mask = bit32.bor(mask, BTN_STRAFE_R)
  elseif rud < -STICK_DEADBAND then
    mask = bit32.bor(mask, BTN_STRAFE_L)
  end
  if getValue(FIRE_SW) > 0 then
    mask = bit32.bor(mask, BTN_FIRE)
  end
  if getValue(USE_SW) > 0 then
    mask = bit32.bor(mask, BTN_USE)
  end

  -- send on change, plus a heartbeat so the dongle knows we're alive
  if mask ~= lastMask or (now - lastSendTime) > 20 then
    sendMask(mask)
    lastMask = mask
    lastSendTime = now
  end

  lcd.clear()
  lcd.drawText(2, 2, "DOOM controller", INVERS)
  lcd.drawText(2, 14, "mask: " .. mask)
  lcd.drawText(2, 24, "ele " .. ele .. "  ail " .. ail)
  lcd.drawText(2, 34, "fire " .. getValue(FIRE_SW) .. "  use " .. getValue(USE_SW))
  lcd.drawText(2, 46, "EXIT long: leave tool")

  return 0
end

return {init = init, run = run}
