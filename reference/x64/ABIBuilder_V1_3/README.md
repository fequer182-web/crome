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

1. Copie para `profiles\` o JSON mais novo salvo pela V1.2. Isso é recomendado porque ele já contém as evidências que você acabou de coletar.
2. Rode `build.bat`.
3. Injete `Build\OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE.dll`.
4. Faça as ações pedidas normalmente.
5. No resumo, procure linhas como `caller site -> 0x... validated=YES`.
6. Envie o console completo e o JSON atualizado.

Se você não copiar o JSON atualizado, os profiles de referência continuam permitindo uma nova calibração, mas você perde a vantagem de carregar as evidências V1.2 já salvas.

## Próximo estágio

Depois de CALL site/caller estáveis, o estágio seguinte pode aprender **como o caller constrói cada argumento antes do CALL**, ainda passivamente, e persistir essas fontes no JSON para automatizar migrações entre builds.
