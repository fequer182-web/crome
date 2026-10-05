# Ramonia x86 Research

Esta pasta concentra a pesquisa da build Ramonia x86 e é a base oficial para as próximas versões do probe.

## Fonte canônico

`src/RamoniaResearchProbe.cpp`

Apesar do arquivo histórico ter usado o nome `OTCLab_BuildMigrator_V4_9_DIRECT_BIND_x86.cpp`, o conteúdo atual corresponde ao **Research Probe V33.1**.

## Build rápido no Windows

Execute:

`build_x86.bat`

O script:

1. procura um compilador MinGW **x86/i686**;
2. recusa compiladores x64;
3. compila `src/RamoniaResearchProbe.cpp`;
4. gera uma pasta limpa em `build/Ramonia_x86_Test/`;
5. copia o profile canônico para `profiles/ramonia_x86/`;
6. copia o injector x86 e `libwinpthread-1.dll` quando disponíveis;
7. abre a pasta de teste ao concluir.

Saída esperada:

`build/Ramonia_x86_Test/RamoniaResearchProbe_V33_1.dll`

> A pasta `build/` é gerada localmente e não é versionada pelo Git.

## Estrutura

- `src/` — fonte canônico do probe atual.
- `profiles/ramonia_x86/` — profile persistente por fingerprint da build.
- `evidence/current/` — snapshots/resultados atuais não canônicos.
- `evidence/history/` — evidências antigas preservadas para comparação.
- `evidence/reference/` — manifests de referência offline.
- `tools/injector/` — injector x86 e dependência runtime usada nos testes atuais.
- `artifacts/legacy/` — DLLs/binários antigos preservados apenas como referência.

## Profile canônico

`profiles/ramonia_x86/6AB03F94_E281E9CA5BA0AF2.json`

Fingerprint:

- PE32 / x86
- timestamp `0x6AB03F94`
- SizeOfImage `0xAA2000`
- EntryPoint RVA `0x47F460`
- .text FNV1A64 `0xE281E9CA5BA0AF2`
- OpenGL

## Estado do probe

O source atual contém:

- descoberta PE/RTTI e semantic binds;
- validação de Game/Map owners;
- layouts de LocalPlayer, Creature, Inventory, Container, UIMap/MapView;
- Tile::things V30;
- Game Action ABI Static Probe V32;
- Game Action ABI Runtime Capture V33.1;
- breakpoint temporário de um byte por ação;
- restauração do byte original no hit;
- `ReadProcessMemory` para snapshots do contexto;
- nenhuma chamada interna de ação feita pelo probe.

## Compatibilidade de profile

O serializer ainda grava o identificador legado `otclab-ramonia-x86-research-v27` / `profileVersion: 27`.
Isso **não representa a versão real do probe**. O parser atual valida a build por timestamp + hash e não usa esse número para aceitar/rejeitar o profile.

Não alterar esse metadado junto com mudanças funcionais. A migração de schema deve ser feita em commit separado e testada.

## Próximo passo

Usar este source como baseline e retomar o teste do runtime ABI capture V33.1, começando por `autoWalk`. Depois das capturas válidas, classificar os argumentos antes de promover qualquer assinatura ativa.
