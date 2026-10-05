# Crome Ramonia Pipe Bridge V1

Primeiro IPC oficial do Crome para Ramonia x86.

## Escopo desta etapa

A bridge e propositalmente READ-ONLY. Ela nao chama nenhuma action interna.

Comandos suportados:

- `HELLO`
- `BUILD_INFO`
- `GET_PLAYER`

O endpoint e criado por PID:

`\\.\pipe\Crome.Ramonia.<PID>`

O processo externo pode ter arquitetura diferente da DLL. O teste oficial compila
`CromePipeTest_x64.exe` e conversa com `CromeRamoniaBridge_x86.dll`.

## Fonte de verdade

A bridge le o profile V37 em `profiles/ramonia_x86/` e usa os offsets do JSON
para localizar Game, LocalPlayer, HP, mana e Position. Nenhum offset de player
fica hardcoded no cliente externo.

## Proximo passo

Depois que o pipe + snapshot estiver validado, o protocolo ganha comandos de action
e um dispatcher interno, mantendo o Crome.exe desacoplado da ABI x86/x64.
