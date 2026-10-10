# Olders: restauracao do runtime historico pelo Pipe

O historico desta conversa registra que a versao de leitura (6689591) bloqueou
a injecao enquanto o catalogo novo estava pendente. O codigo antigo nao foi
removido: OldersInternalActions e olders_walker_test preservam o SDK v21.6.
O catalogo de descoberta x64 (ba9a015) ainda nao implementava o transporte Pipe.

Esta restauracao usa os wrappers e modulos existentes: caminhada/Cavebot,
targeting/ataque, healing, uso de itens, useWith no chao (rope/shovel/fishing),
fala/magias, food e as ferramentas ja executadas pelo worker. As funcoes stub,
anti-idle pendente, backend de move/equip e spear x64 nao ganham implementacao.
Nenhuma flag callable do ABIBuilder e promovida por esta restauracao.

A excecao historica e restrita a timestamp 6AA9FA57, SizeOfImage 17BF000 e hash
FNV1A64 do .text em disco 89D3D35243890EA2, em PE64/AMD64. UI verifica o profile,
personagem/vtable e executavel; DLL verifica o executavel novamente antes dos
wrappers e conserva validateRuntime. Outras builds ficam pendentes.

Pipe local: `Crome.Olders.<PID>`, protocolo 1. STATUS so sinaliza ready depois
do dispatcher instalado. UI nao trata DLL carregada como runtime conectado.
SET_CONFIG envia as configuracoes existentes e recebe confirmacao. A DLL grava
os JSONs atomicamente para os mesmos modulos existentes consumirem; esta versao
conserva o processamento legado por configuracao, nao cria comandos individuais
de acao no protocolo do Ramonia. Consultas/look results continuam nos JSONs.

Config names sao limitados; mensagens ate 256 KiB, clientes remotos rejeitados,
I/O com tempo limitado e cancelamento ao parar. Desconexao nao habilita modulos.
Nao altera configuracoes do usuario instalado durante o desenvolvimento.

Startup deixou de reabrir stdout/stderr do processo. Log e Pipe usam pasta nativa
Unicode; caminhos ANSI legados sao separados sem conversao filesystem incorreta.
Timeout de LoadLibrary nao libera memoria ainda em uso pela thread remota.
Essas correcoes nao confirmam a causa original do fechamento do Olders.

Validacao local: Crome.exe e CromeRuntime.dll compilados; olders_pipe_tests cobre
fingerprint, prontidao, entrega repetida por Pipe real, persistencia/ACK, caminho
com acento, rejeicao de traversal/config invalida e parada limitada. Os wrappers
antigos nao foram modificados. A inicializacao e as acoes reais dependem de teste
no Olders. Reinicie o cliente ao substituir a DLL e use a pasta nova do pacote.
