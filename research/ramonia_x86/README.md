# Ramonia x86 Research

Esta pasta concentra a pesquisa da build Ramonia x86 e é a base oficial para as próximas versões do probe.

## Fonte canônico

`src/RamoniaResearchProbe.cpp`

Apesar do arquivo histórico ter usado o nome `OTCLab_BuildMigrator_V4_9_DIRECT_BIND_x86.cpp`, o conteúdo atual corresponde ao **Research Probe V35**.

## Build rápido no Windows

Execute:

`build_x86.bat`

O script:

1. procura um compilador MinGW **x86/i686**;
2. recusa compiladores x64;
3. compila `src/RamoniaResearchProbe.cpp`;
4. gera uma pasta limpa em `build/Ramonia_x86_Test/`;
5. copia o profile canônico para o pacote de teste;
6. compila o injector x86 canônico de forma estática;
7. abre a pasta de teste ao concluir.

Saída esperada:

`build/Ramonia_x86_Test/RamoniaResearchProbe_V35.dll`

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
- Game Action ABI Runtime Capture V33.1 + ABI Classifier V34;
- breakpoint temporário de um byte por ação;
- restauração do byte original no hit;
- `ReadProcessMemory` para snapshots do contexto;
- nenhuma chamada interna de ação feita pelo probe.

## Compatibilidade de profile

O serializer ainda grava o identificador legado `otclab-ramonia-x86-research-v27` / `profileVersion: 27`.
Isso **não representa a versão real do probe**. O parser atual valida a build por timestamp + hash e não usa esse número para aceitar/rejeitar o profile.

Não alterar esse metadado junto com mudanças funcionais. A migração de schema deve ser feita em commit separado e testada.

## Próximo passo

Usar este source como baseline e capturar as ações reais no V34 para classificar automaticamente os argumentos e persistir o bloco `actions` no profile. O probe continua sem executar chamadas internas.


## V35 — fechamento da ABI x86

O V35 mantém o breakpoint one-shot reversível da V33.1 e não chama ações internas por conta própria.

Ele fecha as ações que ainda estavam apenas `runtime_observed` usando evidência estrutural da própria chamada real:

- `autoWalk`: valida o vector de direções e seus elementos;
- `look/use/move/useWith`: reconhece referências a `shared_object_ptr` por stack-local holder + RTTI;
- `move`: reconhece `Position` OTC em layout compacto e o count escalar;
- ações já `runtime_confirmed` no profile são puladas automaticamente.

Quando executado dentro de `build/Ramonia_x86_Test`, cada save também espelha o profile para o caminho canônico `profiles/ramonia_x86/`. Assim o GitHub Desktop passa a mostrar o JSON atualizado sem cópia manual.
