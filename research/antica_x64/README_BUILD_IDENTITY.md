# Identidade de build baseada no codigo — Antica x64

O verificador consulta somente o arquivo PE no disco. Nao le nem escreve memoria; nao instala hooks nem injeta DLL.

## Regra

1. Validar x64, PE timestamp, SizeOfImage, secao .text RVA e tamanho raw.
2. Validar SHA-256 da secao .text como hash principal e FNV1A64 como hash legado.
3. Guardar SHA-256 completo do EXE apenas como identificador de instalacao: nao bloqueia se .text e layout sao compativeis.
4. Qualquer divergencia de codigo ou layout bloqueia o perfil.
5. Identidade de arquivo nao substitui validacao runtime de VTables, offsets, thread e ABI. nativeActionsReady=false.

## Executar

Copie build_identity.py e antica_identity.json para a mesma pasta.

    py -3 build_identity.py --pid 25360

ou

    py -3 build_identity.py --file "C:\Users\alex1\Downloads\Antica Retro\Antica Retro\Antica Retro opengl.exe"

Os resultados sao gravados em preflight_result.json.

O comando --pid utiliza apenas PROCESS_QUERY_LIMITED_INFORMATION; nao solicita PROCESS_VM_READ.
Se o cliente negar leitura de memoria com WinError 5, nao contorne essa restricao; as acoes nativas continuam desabilitadas.

## Testes locais (Python)

- PE original 0x6B06FF0F: compativel.
- PE com bytes extras apos o arquivo e .text igual: compativel, instalacao diferente.
- PE com 1 byte modificado em .text: incompativel.

Este projeto fica isolado do Crome.exe e dos perfis nativos de outras builds.
