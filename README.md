# Pearl GPU Probe para Xbox Series S

Protótipo de desenvolvimento preparado a partir de um Mac. Gera provas densas
V3 completas e inclui uma sessão experimental de mineração direta por TLS,
ativada somente por configuração privada. **Ainda não há share aceita ou receita
demonstrada.** Sem essa configuração, executa apenas o diagnóstico offline.

## O que está implementado

- BLAKE3 portátil em C, obtido do projeto oficial e fixado por versão e SHA-256.
- Derivação das sementes de ruído de provas Pearl V3 para matrizes densas.
- Geração de matrizes, ruído de consenso, árvores BLAKE3 e aberturas de linhas.
- Serialização canônica de `PlainProof` densa V3, rank 128 e blocos de 16×16.
- Referência de CPU para multiplicação exata de inteiros com sinal, redução XOR
  cumulativa a cada 128 termos e acumulação de 16 palavras com rotação de 13 bits.
- Shader HLSL equivalente, com entradas de 8 bits empacotadas e acumulador de
  32 bits; compilação antecipada para `cs_5_1`.
- Aplicativo UWP x64 separado do xllama, com apenas a capacidade `internetClient`.
- Sessão TLS com autorização, tarefas V3, descarte de trabalho antigo e contagem
  de shares baseada nas respostas do pool. O Mac não executa mineração.
- Duração limitada para testes ou operação contínua explicitamente ativada
  na configuração privada, com reconexão de rede e encerramento ao fechar o app.
- Preparação em 1 a 6 threads de CPU, sobreposta ao cálculo na GPU. A fila
  tem tamanho limitado e descarta trabalho preparado para tarefas antigas.
- Tempos separados de preparação, espera, execução GPU e análise dos resultados.
  O tempo do processo também permite medir o uso agregado da CPU.
- GPU com linhas de memória compartilhada espaçadas, leitura de quatro bytes
  por thread e blocos de 64 termos entre sincronizações. A mineração transfere
  de volta apenas as transcrições; o diagnóstico mantém o produto completo.
- Receita de compilação Windows acionada pelo Mac via GitHub Actions.

O diagnóstico original de aritmética permanece disponível no código. O teste
atual produz três provas completas com cabeçalho e sementes fixos. São exemplos
offline: **não correspondem a uma tarefa vigente ou a um alvo real do pool**.

## Validação local

No Mac, a compilação Release e os **95 checks** de referência passaram. Incluem
35 vetores oficiais BLAKE3 em dois modos, os dois vetores oficiais de sementes
V3 da Pearl e casos de aritmética negativa/acumulação cumulativa. Isso valida
essas partes, não a correção de um minerador completo. As três provas C++ foram
também aceitas por `verify_plain_proof` e `SeedDerivation::Salted` do código Rust
oficial fixado abaixo, usando alvo máximo de teste. A transcrição e o hash final
coincidiram; cinco alterações deliberadas foram rejeitadas. Esse teste não valida
dificuldade de uma share de produção.

```sh
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --parallel 4
ctest --test-dir build-mac --output-on-failure
```

## Pacote Xbox

O fluxo `.github/workflows/build-xbox.yml` é **manual**. Ele usa um executor
Windows 2022 padrão, tem limite de 15 minutos e guarda o pacote por um dia.
Executa compilação e testes curtos, sem mineração no servidor. A verificação
da franquia/cobrança ou a autorização para publicação deve anteceder seu uso.

O pacote usa Windows SDK 22621, MSVC v143 e C++/WinRT 2.0.240405.15. O certificado
de desenvolvimento é gerado no servidor de compilação; a chave privada não é
incluída no resultado. O certificado público e o SHA-256 acompanham o pacote.

Depois de instalar pelo Device Portal, definir **Pearl GPU Probe** como **Game**
no Dev Home e iniciar. O teste ativa uma janela sem interface, executa uma vez,
escreve `pearl-probe-result.json` na própria pasta `LocalState` e encerra.
A aplicação xllama e seus modelos ficam em outro pacote.

O teste compara **todas as palavras** de saída da GPU com a referência CPU:
produto final e transcrição cumulativa, usando três tamanhos de matriz. Salva
também `pearl-proof-header.bin` e `pearl-proof-0.bin` até `pearl-proof-2.bin`,
para verificação externa. Registra tempos da GPU e do envio/espera separadamente.
Adaptadores de software são rejeitados. Divergência, falha de dispositivo ou
espera de GPU acima de 15 segundos interrompe o teste.

Um resultado `passed` demonstra a equivalência CPU/GPU desses exemplos. A
validade da prova completa precisa ainda da verificação independente dos
arquivos exportados. Nenhum dos dois resultados significa share aceita,
rentabilidade ou estabilidade 24 horas. As otimizações mantêm os checkpoints
de consenso e precisam passar pela mesma comparação exata e verificação externa.

## Validação ainda pendente

As três provas completas exportadas pelo Xbox já passaram no verificador Rust
oficial; são idênticas, byte a byte, às provas C++ de referência. Cinco alterações
deliberadas nos arquivos do Xbox também foram rejeitadas.

Provas de tarefas atuais, recebidas diretamente no Xbox, também passaram na
verificação independente de estrutura e matemática. Isso não demonstra que uma
prova atingiu a dificuldade real ou recebeu crédito do pool.

1. Confirmar a interpretação do alvo pela aceitação de uma share real.
2. Obter trabalho aceito pelo pool e medir a taxa efetiva.
3. Só então comparar receita e testar continuidade prolongada.

## Sessão experimental direta

A configuração `pearl-mining-config.json`, fornecida privadamente na pasta
`LocalState`, precisa conter `mining_enabled: true`, `host`, `port: 8048`,
`wallet`, `worker`, `pass`, `m`, `n`, `k` e `run_seconds` (1 a 86400).
Para operação contínua, definir explicitamente `continuous: true` e
`run_seconds: 0`; nessa modalidade não há encerramento automático por tempo.
Sem `continuous`, a duração padrão continua sendo 120 segundos.
`preparation_workers` controla a preparação CPU (1 a 6; padrão 3).
M e N aceitam múltiplos de 16 até 2048; cada envio à GPU está limitado a
2^34 unidades de trabalho. Aumentar dimensões ou threads exige medir o ganho
e conferir as provas; esses parâmetros não alteram a frequência do hardware.
O aplicativo conecta diretamente ao domínio Kryptex por TLS com validação
normal do certificado. Nenhuma conta é incluída no pacote ou no repositório.

O primeiro bloco calculado gera `pearl-live-header.bin`, `pearl-live-proof.bin`
e `pearl-live-sample.json` para verificação externa; essa amostra não é enviada
ao pool. A sessão compara uma transcrição real com a CPU antes de habilitar
envios e verifica cada candidato antes de submetê-lo. Só envia candidatos que
atingem o alvo base anunciado multiplicado pelo fator de trabalho `256*K`.
Essa interpretação precisa ainda ser confirmada por aceitação real no pool.

`pearl-mining-status.json` separa tentativas, unidades de trabalho calculadas,
envios e aceitações. As unidades locais **não são hashrate efetivo creditado**.
`gpu_kernel_duty_percent` é a fração de tempo coberta pelos timestamps dos
kernels deste processo, não uma contagem de CUs ocupadas. O tempo de CPU
agregado é expresso em equivalentes de processadores lógicos ocupados;
não demonstra acesso a toda a CPU física do console.
Erros de conexão geram reconexão; versão de certificado diferente de 3 ou
divergência de cálculo interrompem a sessão, inclusive em modo contínuo.
A operação requer foreground: fechar o aplicativo ou reiniciar o console
interrompe a mineração. O aplicativo não instala um mecanismo de inicialização
automática após reinício. Operação contínua habilitada não equivale a estabilidade
de 24 horas já demonstrada.

Dados de conta/pagamento, respostas privadas do pool, endereço do console,
capturas de tela e diagnósticos pessoais **não fazem parte deste projeto**.

## Fontes fixadas

- [Pearl](https://github.com/pearl-research-labs/pearl/tree/5b09d844e4069440933722c51495ac24a7bb4886):
  sementes, ruído, provas, verificação e árvores Merkle (arquivos e hashes em
  `third_party/sources.json`).
- [BLAKE3](https://github.com/BLAKE3-team/BLAKE3/tree/6aab490a26124663329dfd3961b8469f8fdb158b):
  implementação C portátil e vetores de teste oficiais.
- [xllama](https://github.com/gianlucamazza/xllama/tree/3d2cfae8b81fbdece799228538366650e1c41338):
  referência de compilação UWP e carregamento tardio do D3D12 já testados em Xbox.

Licenças dos componentes/regras portadas e checksums estão em `third_party/`.
