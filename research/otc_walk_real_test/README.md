# Crome — Caminhada real de 2 SQMs no OTC/TFS controlado

## Finalidade

Teste **real de movimentação do personagem** pelo motor do seu OTC/TFS, não por simulação de teclado. A API de mapas do OTClient utiliza `player:autoWalk(autoWalkPos)` quando um jogador clica em um destino; este módulo chama a mesma API para o destino `X+2, Y, Z`.

**O que ele NÃO faz:** injeção de DLL no Antica, acesso externo de memória, chamada direta ao RVA `0x140D00`, prova de ABI nativa ou ligação entre a bridge x64 e o cliente OTC. O arquivo .bat instala um módulo, não executa a caminhada diretamente.

## Instalação

1. Feche seu OTC de testes.
2. Execute `01_INSTALAR_NO_OTC.bat`.
3. Informe a pasta `mods` do OTC, por exemplo `D:\OTC_Teste\Client OTC\mods` (adapte à sua instalação).
4. Abra o OTC, entre no seu servidor TFS controlado e vá para um local com dois SQMs livres à direita do mapa (Leste).
5. Pressione **Ctrl+F11 uma vez**, com o foco na janela do jogo.
6. Veja as mensagens "[CROME WALK TEST]" na interface/log do cliente.

O módulo não caminha automaticamente ao fazer login; exige acionamento explícito. O script somente envia **um** pedido e verifica a posição depois de cinco segundos. Se não houver dois tiles livres, bloqueia o teste.

## Exemplo esperado

```text
[CROME WALK TEST] ANTES 32100,32196,7 -> DESTINO 32102,32196,7
[CROME WALK TEST] autoWalk solicitado uma vez. Conferindo posicao...
[CROME WALK TEST] DEPOIS 32102,32196,7
[CROME WALK TEST] SUCESSO: personagem moveu exatamente 2 SQMs a Leste.
```

Se aparecer "FALHOU" ou "MOVIMENTO PARCIAL", envie a saída e indique se os SQMs estavam livres. A posição é apenas exemplo.

## Compatibilidade

As implementações de OTC/OTCv8 diferem. Esta primeira versão usa `g_keyboard.bindKeyDown`, `g_game.getLocalPlayer()`, `g_map.getTile` e `player:autoWalk` encontrados em variantes OTC. A efetiva execução e comportamento no seu cliente 7.72 **ainda não foram validados**.

Este teste é separado da PR da DLL x64. Para testar a **DLL conectada ao motor** precisaremos de um carregador autorizado pela build OTC controlada e um adaptador no próprio processo, não apenas este script.
