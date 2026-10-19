MFGAdaUnlock - Dual Backend X1.2 (base oficial 1.4.5)

Preserva os redirecionamentos de API que pertencem ao OptiScaler, sem inserir
outro wrapper que mude a identificacao do chamador. Contextos temporarios ja
destruidos deixam de bloquear MFG; um contexto nativo ainda vivo continua
impedindo a publicacao global do modelo. Mantem a correcao de startup da X1.1.
O modelo, os shaders e os ajustes de imagem nao foram alterados nesta correcao.

DLSSG: base de geracao/qualidade 1.4.5, com ReShade direto.
XeMFG: OptiScaler 10, modelo MFG Intel adaptado, XeLL e pacing dos frames extras.
O port desta candidata e admitido em NVIDIA com os runtimes Intel revisados.
O plugin MFG anterior deve ficar guardado; nao carregue os dois unlockers juntos.

Com o jogo fechado, execute no PowerShell a partir deste pacote:
.\Switch-MFGBackend.ps1 -GameDirectory "<pasta do executavel do jogo>" -Backend XeMFG
Para usar DLSS-G, execute o mesmo comando com -Backend DLSSG.
O script verifica os hashes e faz backup dos arquivos atingidos antes da troca.
O RenoDX do jogo e os ajustes de upscaling/imagem sao preservados.

Para voltar exatamente a uma instalacao salva, com o jogo fechado:
.\Restore-MFGBackup.ps1 -BackupDirectory "<pasta backup indicada pelo seletor>"
O backup inicial do Cyberpunk permanece em MFGAdaUnlock-profiles na pasta x64.
Para voltar ao addon oficial 1.4.5, selecione primeiro DLSSG e substitua o addon
pelo arquivo da pasta rollback, usando o nome renodx-mfgunlock.addon64.

Validacao: 45 testes aprovados; 3 opcionais pulados. Inclui abertura com
OptiScaler real, ciclo de vida XeLL/XeFG e contagens 2x/3x/4x/6x em host
sintetico, caller dos redirects, Present/resize e protecao de contexto vivo.
A X1.1 passou do startup e caiu durante Present no XeLL no Cyberpunk.
Esta candidata corrige duas falhas reproduzidas offline; a eliminacao daquele
crash e o funcionamento com imagens geradas ainda aguardam teste no jogo.
Veja engineering-report.md, manifest.json, evidence/, PROVENANCE.md e notices/.
Este e um pacote local; nenhuma nova release foi publicada.
