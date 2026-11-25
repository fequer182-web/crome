# Crome Antica: experimento de bridge x64 — caminhada relativa

## Escopo

Este experimento constrói uma DLL x64 com uma API de comando
`CromeWalkBridge_WalkRelative(2,0,0)`. O **TestHost** carrega a DLL
em um processo de teste próprio e fornece callbacks fictícios
de leitura da posição e pedido de caminhada.

O objetivo é testar **LoadLibrary em processo autorizado + ABI entre
host e DLL + cálculo de destino + exatamente um pedido**.

**Não injeta no Antica e não movimenta Momonga.** Não há hooks,
leitura/escrita remota, mitigação de anticheat, nem chamada ao RVA
candidato 0x140D00. A ABI real de `Game::autoWalk` não foi confirmada.

## Executar no Windows

Abra o CMD na pasta deste experimento:

`01_COMPILAR_E_TESTAR.bat`

Exige MSYS2 UCRT64 g++ (mesmo ambiente já usado para compilar Crome).

Saída esperada:

```text
[MOCK] ANTES  : 32963,32109,6
[MOCK] Pedido de destino: 32965,32109,6
[MOCK] DEPOIS : 32965,32109,6
[OK] DLL x64, API e WALK_REL(2,0,0) validados no HOST DE TESTE.
[INFO] Nenhum personagem real foi movido.
```

## Próximas validações

1. Confirmar uma forma de carregar uma DLL **autorizada** no cliente de teste.
2. Ligar o callback real ao motor de caminhada em cliente controlado.
3. Validar a thread e a ABI com o destino de 2 SQMs.
4. Não reutilizar indiscriminadamente as RVAs do Antica: são evidências
   estáticas, ainda não confirmadas em runtime.
