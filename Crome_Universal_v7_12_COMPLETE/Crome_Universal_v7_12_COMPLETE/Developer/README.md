# Crome - Developer

Esta pasta contém somente o material necessário para desenvolver e gerar o executável.

## Pastas principais

- `core/` — profile, runtime, readers, snapshots e ações.
- `app/` — controller, scheduler e cooldowns.
- `modules/` — Healing, Support, RuneMaker, Targeting, Cavebot e Looter.
- `ui/` — interface ImGui.
- `api/` — API interna estável do Crome.
- `profiles/` — BuildProfile v2 usados pelo Core.
- `config/` — configurações padrão.
- `docs/`, `tests/`, `tools/` — apoio apenas para desenvolvimento.

## Gerar Crome.exe

Execute `build.bat`.

O script:
1. configura o CMake;
2. compila somente o target principal `Crome`;
3. gera `build\bin\Crome.exe`;
4. copia o executável para `..\Crome\Crome.exe`;
5. sincroniza os profiles;
6. preserva configs já existentes na pasta do usuário.

`clean.bat` remove somente a pasta de build de desenvolvimento. A distribuição `..\Crome` é preservada.

## Separação

A pasta `Developer` não deve ser entregue ao usuário final. A distribuição final é apenas `..\Crome`.

## v7.7 Runtime Guard
Se `CromeRuntime.dll` nao conectar, a interface mostra uma mensagem bloqueante com **Retry Runtime**. Os modulos nativos nao podem ser habilitados enquanto o runtime estiver desconectado. O catalogo global `Crome/config/walker_ids.json` e preservado entre rebuilds.


## v7.8 Look ID
O runtime monitora o `Game::look` confirmado do profile Olders. O resultado aparece em `Crome/config/look_result.json` e no rodapé da UI. A janela IDs permanece apenas para edição manual.

## v0.7.9 - x64 Look ID + HackLight
Os profiles agora incluem `features.look` e `features.hackLight`. Consulte `docs/X64_LOOK_LIGHT_PROFILE.md`.
Use `build.bat tests` para compilar o release e os probes x64.
