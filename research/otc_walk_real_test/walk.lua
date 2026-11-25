-- Crome / OTC-TFS test: issue exactly one 2-SQM autoWalk request.
-- Controlled OTC client only. Not an Antica injection or a native DLL call.
local HOTKEY = 'Ctrl+F11'
local pending = false
local verificationEvent = nil

local function notify(message)
  local line = '[CROME WALK TEST] ' .. message
  print(line)
  if modules and modules.game_textmessage then
    local text = modules.game_textmessage
    if text.displayStatusMessage then
      text.displayStatusMessage(line)
    elseif text.displayGameMessage then
      text.displayGameMessage(line)
    end
  end
end

local function equals(a, b)
  return a and b and a.x == b.x and a.y == b.y and a.z == b.z
end

local function runTest()
  if pending then
    notify('Teste em andamento; nenhum novo comando enviado.')
    return
  end
  if not g_game.isOnline() then
    notify('Entre no personagem antes de executar.')
    return
  end
  local player = g_game.getLocalPlayer()
  if not player then
    notify('LocalPlayer ainda indisponivel.')
    return
  end
  local before = player:getPosition()
  if not before then
    notify('Posicao inicial indisponivel.')
    return
  end
  local middle = {x = before.x + 1, y = before.y, z = before.z}
  local destination = {x = before.x + 2, y = before.y, z = before.z}
  local tile1 = g_map.getTile(middle)
  local tile2 = g_map.getTile(destination)
  if not tile1 or not tile2 or
     not tile1:isWalkable() or not tile2:isWalkable() then
    notify('Os dois SQMs a Leste nao estao livres/visiveis.')
    return
  end

  notify(string.format('ANTES %d,%d,%d -> DESTINO %d,%d,%d',
    before.x, before.y, before.z, destination.x, destination.y, destination.z))

  pending = true
  -- Same high-level action used by OTClient map click (not keyboard walking).
  local ok, err = pcall(function()
    player:autoWalk(destination)
  end)
  if not ok then
    pending = false
    notify('ERRO ao solicitar autoWalk: ' .. tostring(err))
    return
  end
  notify('autoWalk solicitado uma vez. Conferindo posicao...')
  verificationEvent = scheduleEvent(function()
    verificationEvent = nil
    pending = false
    local current = g_game.getLocalPlayer()
    local after = current and current:getPosition()
    if not after then
      notify('Sem personagem para validar o resultado.')
      return
    end
    notify(string.format('DEPOIS %d,%d,%d', after.x, after.y, after.z))
    if equals(after, destination) then
      notify('SUCESSO: personagem moveu exatamente 2 SQMs a Leste.')
    elseif equals(after, before) then
      notify('FALHOU: personagem nao saiu do lugar.')
    else
      notify('MOVIMENTO PARCIAL/DIFERENTE: pedido foi enviado, destino nao confirmado.')
    end
  end, 5000)
end

function init()
  g_keyboard.bindKeyDown(HOTKEY, runTest)
  if g_game.isOnline() then
    notify('Pronto. Fique em area aberta e pressione ' .. HOTKEY .. ' uma vez.')
  end
  connect(g_game, {
    onGameStart = onGameStart,
    onGameEnd = onGameEnd
  })
end

function onGameStart()
  notify('Modulo carregado. Em area aberta, use ' .. HOTKEY .. ' uma vez.')
end

function onGameEnd()
  pending = false
  if verificationEvent then
    removeEvent(verificationEvent)
    verificationEvent = nil
  end
end

function terminate()
  g_keyboard.unbindKeyDown(HOTKEY)
  disconnect(g_game, {
    onGameStart = onGameStart,
    onGameEnd = onGameEnd
  })
  if verificationEvent then
    removeEvent(verificationEvent)
    verificationEvent = nil
  end
  pending = false
end
