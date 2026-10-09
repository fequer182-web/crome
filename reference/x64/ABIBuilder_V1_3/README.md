# OTCLab ABI Builder V1.3 X64 — AutoCalibrator / Caller Evidence

O padrão do pipe exige o JSON recém-gerado pelo BuildExplorer atualizado, com `pipeDiscovery` e fingerprint exata. Profiles legados sem esse catálogo não autorizam captura. Todas as funções continuam passivas (`callable=false`). Veja [fluxo e pendências](../PIPE_DISCOVERY.md).

A V1.3 continua **100% passiva**: não chama funções internas. O jogador só produz ações reais no cliente e o Builder coleta e combina as evidências automaticamente.

## O que a V1.3 adiciona

Além dos snapshots de `RCX/RDX/R8/R9` da V1.2, cada hit agora tenta resolver automaticamente:

- `returnRva`;
- `naturalCallSiteRva`;
- tipo do CALL (`call_rel32` ou `call_indirect`);
- início/fim da função caller usando a exception directory (`.pdata`) do PE x64;
- janela de bytes ao redor do CALL;
- prefixo da função caller;
- consistência do mesmo CALL site entre estímulo A e B.

Tudo é gravado no `abiCatalog` schema v4 do mesmo JSON.

## Regra de promoção

`callerCandidateReady=true` somente quando a ABI foi validada, a thread natural é consistente e o mesmo CALL site apareceu nos dois estímulos A/B. `callable` continua **false**.

## Uso

1. Copie para `profiles\` o JSON mais novo salvo pelo BuildExplorer atualizado, contendo `pipeDiscovery` e a fingerprint exata do cliente.
2. Rode `build.bat`.
3. Injete `Build\OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE.dll`.
4. Faça as ações pedidas normalmente.
5. No resumo, procure linhas como `caller site -> 0x... validated=YES`.
6. Envie o console completo e o JSON atualizado.

Profiles antigos sem o catálogo de descoberta atual são recusados. Evidências ABI anteriores só são preservadas com a mesma fingerprint e o mesmo RVA redescoberto.

## Diagnóstico de fechamento ao injetar

A DLL grava `ABIBuilderStartup.log` ao lado dela assim que a thread de trabalho inicia. O arquivo inclui etapas da inicialização e uma cópia do console. A ausência do arquivo pode significar falha antes dessa thread ou falta de permissão de escrita; não prova uma causa específica.

O console usa handles próprios do Windows, sem `freopen` e sem alterar `stdin`, `stdout`, `stderr` ou os handles padrão do cliente. Seus buffers C++ são restaurados antes de descarregar. O build também incorpora a dependência de threads do compilador, evitando carregar `libwinpthread-1.dll` do cliente ou do injector.

Para testar, abra uma sessão nova do Olders, sem Crome/BuildExplorer/outras probes carregadas, e injete somente esta ABIBuilder. Primeiro confirme que o console chega à pergunta inicial. Se fechar, envie as últimas linhas do log. Compilação e testes locais não confirmam a resolução de um crash no cliente real.

Os testes `tests/StartupIoTests.cpp` verificam leitura/escrita guiada, preservação dos streams do host, restauração dos buffers inclusive após exceção e ausência de breakpoint na inicialização. As funções permanecem `callable=false`.

Os caminhos de DLL e profiles permanecem em UTF-16 nativo do Windows durante busca, leitura, gravação e backup. A apresentação no console usa UTF-8. `tests/UnicodeProfileTests.cpp` cobre pastas como `Área de Trabalho` e nomes de arquivo com acentos, sem depender da configuração de locale do cliente.

## Look normal e detailedLook

O assistente testa primeiro o candidato `Game::look` redescoberto para a build atual, pois o estímulo pedido é o Look normal. Se não observar nenhuma chamada, testa `Game::detailedLook` separadamente, quando também descoberto. Uma captura parcial não é combinada com outra função. A chave do catálogo e o RVA reutilizado por Move/Use/UseWith são os da função realmente capturada. Um caminho de Item ambíguo mantém o assistente pendente; `callable` permanece falso.

`tests/LookSelectionTests.cpp` cobre seleção normal, fallback separado, identidade do catálogo/cache e rejeição de captura parcial ou descoberta de outra build.

## Próximo estágio

Depois de CALL site/caller estáveis, o estágio seguinte pode aprender **como o caller constrói cada argumento antes do CALL**, ainda passivamente, e persistir essas fontes no JSON para automatizar migrações entre builds.
