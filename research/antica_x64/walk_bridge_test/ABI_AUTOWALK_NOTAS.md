# Antica Retro x64 — achados estáticos sobre `Game::autoWalk`

**Build:** PE Timestamp `0x6B06FF0F`, SizeOfImage `0xF38000`, SHA-256 `.text` `6244c268981a4c46791fbc660c96f0f22644806f4e0494ad5244d48e32a94bb0`.

**Fonte:** `walk_path_report.txt` produzido pelo `03_ANALISAR_CAMINHO_ANTICA.bat` do usuário. A análise é **exclusivamente estática**: não houve chamada à função nem acesso à memória do processo.

## Chamada (RVA 0x15F71E → 0x140D00)

Instruções imediatamente anteriores:

```asm
14015f6f9  mov rdx, qword ptr [rbx]
14015f6fc  movsd xmm0, qword ptr [rdx+0x24]
14015f701  movsd qword ptr [rsp+0x60], xmm0
14015f707  mov eax, dword ptr [rdx+0x2c]
14015f70a  mov dword ptr [rsp+0x68], eax
14015f70e  add rdx, 8
14015f712  lea r8, [rsp+0x60]
14015f717  lea rcx, [rip+0xd49ff2]   ; RVA 0xEA9710
14015f71e  call 0x140140d00
```

Argumentos observados no Windows x64:

- **RCX**: endereço do objeto global `Game` (RVA `0xEA9710`, não ponteiro carregado desse endereço).
- **RDX**: ponteiro para um objeto com o início e o fim de uma sequência de elementos de 4 bytes; construído como `[rbx] + 8`.
- **R8**: ponteiro para uma cópia de 12 bytes em `rsp+0x60`, com dois campos de 32 bits em `+0/+4` e terceiro de 32 bits em `+8` — candidato a `Position`. **O relatório não prova se essa posição deve ser tratada como origem ou destino**; a função a utiliza como base para calcular o primeiro passo.

No corpo da função: `mov r14,r8; mov rsi,rdx; mov rdi,rcx` e `([rsi+8]-[rsi])/4` para contar os elementos. A função não prossegue se o vetor estiver vazio.

## Enumeração de direções deduzida do switch

Em RVA `0x141045`, o programa lê `int32_t` do primeiro elemento do vetor; aceita entradas até `7`. A jump table de 8 DWORDs fica em **RVA `0x1412F8`** e aponta para as rotinas abaixo:

| Código | RVA do case | Efeito nas coordenadas | Direção |
|---:|---:|---|---|
| 0 | `0x14105D` | Y-1 | Norte |
| 1 | `0x14106C` | X+1 | Leste |
| 2 | `0x14107E` | Y+1 | Sul |
| 3 | `0x141071` | X-1 | Oeste |
| 4 | `0x141076` | X+1,Y-1 | Nordeste |
| 5 | `0x14107B` | X+1,Y+1 | Sudeste |
| 6 | `0x141083` | X-1,Y+1 | Sudoeste |
| 7 | `0x14108B` | X-1,Y-1 | Noroeste |

Para um movimento relativo de **2 SQMs ao Leste**, a sequência candidata é `[1,1]`, com coordenada esperada `X+2,Y,Z` após execução. Isso **não** é uma chamada de movimento validada.

## Distinção do Olders

Olders havia validado um wrapper `LocalPlayer::walkTo(Position*, bool)`. Antica usa uma rotina **diferente**, aparentemente `Game::autoWalk(path, Position*)`. Não transplantar diretamente o protótipo do Olders.

## Pendências antes de qualquer experimento nativo

- Determinar o contrato de propriedade/tempo de vida do vetor e semântica da posição (origem versus destino) e demais guardas internos.
- Confirmar thread correta, estado do mundo, objeto Game e limites válidos.
- Obter mecanismo **autorizado** de carregamento e execução no cliente, sem contornar o `WinError 5` observado anteriormente.
- Manter `nativeActionsReady=false`: nenhum SQM foi movimentado com esta descoberta.
