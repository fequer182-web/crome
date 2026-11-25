# Validação do Crome no Antica Retro OpenGL x64

Este registro cobre as builds `0x6B05C35D` e `0x6B06B118`. O Crome lê os dados do cliente em modo externo de somente leitura. Os testes em jogo usam ações feitas pelo jogador e comparam leituras antes e depois de cada ação.

O [perfil externo de leitura](profiles/Antica-6B06B118-leitura.json) foi testado com a interface do `Crome_Antica_Look`. Ele fica nesta pasta de pesquisa, fora dos perfis carregados pela distribuição principal. A compatibilidade com o consumidor principal ainda precisa ser verificada.

## Identificação das builds

| Campo | `0x6B05C35D` | `0x6B06B118` |
| --- | --- | --- |
| Carimbo PE | `0x6B05C35D` | `0x6B06B118` |
| Tamanho da imagem PE | `0xF2A000` | `0xF33000` |
| SHA-256 | `1b935396739dc35c228ec8c6db056e32b44dba46b904d8b28c4dcb26bdd04417` | `c2b09f400d2ed634325b7bb1bdb707c05a3737aa4d26b51dc2453f393699af70` |
| Referência do jogador | `0xE9D6F0` | `0xEA5760` |
| Alvo ativo | `0xE9D700` | `0xEA5770` |
| Árvore de criaturas | `0xE9DBF0` | `0xEA5C60` |
| Base do mapa | `0xE9DAF0` | `0xEA5B60` |
| Árvore de contêineres | `0xE9D730` | `0xEA57A0` |

Os cinco offsets de leitura da tabela são relativos ao módulo. Na build `0x6B06B118`, o FNV-1a de 64 bits da seção `.text` é `0x19646F257541E470`. O perfil identifica a build antes de resolver as referências. A árvore de criaturas, o mapa e os contêineres da build nova foram conferidos em leitura e ficaram registrados como observações do perfil.

## Testes em jogo

| Teste | Resultado observado |
| --- | --- |
| Jogador e reinício | Ambas as builds reencontraram o personagem após reiniciar. Na `0x6B06B118`, a interface original do Crome exibiu vida `210/210` e mana `185/185`. |
| Seleção e cancelamento | Nas duas builds, o alvo passou de vazio para `Rat` quando o jogador atacou e voltou a vazio após cancelar. A sequência foi repetida após reiniciar a `0x6B06B118`. |
| Dano e morte | Na `0x6B05C35D`, o Rat chegou a `0%`, o alvo foi limpo e a XP do personagem aumentou em cinco pontos. |
| Caminhada | Na `0x6B05C35D`, as coordenadas mudaram após um passo pela tecla e durante outro teste com um clique no mapa. O quadrado exato clicado ficou sem confirmação. |
| Uso e transferência de itens | Na `0x6B05C35D`, o ID `3582` saiu da mochila após o consumo do ham. As pilhas de moedas `3031` foram conferidas nos contêineres de origem e destino. |
| Loot | Na `0x6B05C35D`, três moedas `3031` saíram do corpo do Rat e a pilha de destino aumentou de `38` para `41`. O queijo `3607` desapareceu após o consumo e o corpo permaneceu aberto e vazio. |
| Look | Na `0x6B05C35D`, a leitura pontual encontrou `You see a shovel. It weighs 35.00 oz.` depois do Look manual na pá `3457`. A frase estava ausente na leitura inicial. |

Na `0x6B06B118`, também foram conferidos o registro do personagem na árvore de criaturas, o quadrado que o continha e duas pilhas de moedas nas mochilas. Falta registrar uma mudança controlada de coordenadas nessa build. A validação de comandos iniciados pelo Crome, incluindo rota, ataque, uso de item, Look e loot, permanece pendente. O perfil mantém `nativeActionsReady=false`.

## Contexto das versões anteriores

A entrega `Crome_Antica_Look` para a build `0x6AF9EA64` registrou caminhada por destino, ataque ao Rat, cura com `exura` e loot em testes em jogo. O ciclo automático completo ficou pendente. Uma recompilação posterior do Crome ainda trazia o perfil nativo dessa build. Os testes manuais por leitura nas builds da tabela têm escopo próprio e não validam os comandos dessas entregas antigas.

Na `0x6B05C35D`, o Look informado pelo jogador também identificou o ham (`3582`, 20 oz) e a vara de pescar (`3483`, 8,50 oz). A captura diretamente da memória foi feita com a pá. A transferência de pilhas entre mochilas e a transferência de loot do corpo tiveram testes separados.

## Como repetir após atualizar o cliente

1. Identifique o executável x64 pelo carimbo PE, tamanho da imagem e SHA-256. Para o perfil do Crome, confira também a identificação de `.text`, incluindo o hash FNV-1a. Use um perfil específico da build.
2. Abra o Crome ou o painel de diagnóstico de leitura e selecione o processo da sessão atual. Localize o personagem e compare os atributos lidos com o que a interface do jogo mostra. Se precisar localizar o jogador de novo, use nome e XP como referências iniciais. Confira vida, mana e nível no mesmo bloco. Valide a referência relativa ao módulo em outro processo da mesma build antes de usar o perfil.
3. Salve uma leitura inicial. Faça uma ação por vez pelos controles do jogo e salve a leitura seguinte. Para movimento, compare X, Y e Z. Para combate, compare o alvo, a vida da criatura e a XP. Para itens, compare ID e quantidade no corpo ou na mochila de origem e no destino.
4. Para Look, prepare a leitura com uma frase de referência, use Look no item e capture a mensagem. O teste da pá comparou duas leituras pontuais de páginas privadas legíveis. O painel limita cada busca a 768 MiB e 12 segundos. Se o cliente reutilizar o mesmo endereço e a mesma frase, a comparação pode não detectar uma ocorrência nova.
5. Reinicie o cliente e confirme novamente a leitura com a mesma identificação de build. Registre quais campos foram conferidos, qual transição foi observada e o que ainda está pendente.

No código do Crome original, consulte `core/profile/ProfileLoader.cpp` para o carregamento, `core/runtime/ClientSession.cpp` para a identificação da build e `core/readers/ExternalTargetReader.hpp` para a leitura de alvo.
