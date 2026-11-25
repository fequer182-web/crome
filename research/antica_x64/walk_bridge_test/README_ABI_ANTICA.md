## Inspecao offline da ABI Antica

Execute 02_ANALISAR_ABI_ANTICA.bat apontando para o Antica Retro opengl.exe.
Verifica timestamp, tamanho de imagem, SHA-256 da .text, e o CALL em RVA 0x15F71E para Game::autoWalk candidato 0x140D00.
Salva disassembly do caller/callee via objdump e bytes no walk_abi_report.txt.

Este teste NAO injeta DLL, NAO modifica memoria e NAO movimenta o personagem. A assinatura validada anteriormente no Olders pertence a outro wrapper (LocalPlayer::walkTo), e nao deve ser reutilizada sem evidencia.
