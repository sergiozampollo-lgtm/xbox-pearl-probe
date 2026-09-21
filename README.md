# Pearl GPU Probe para Xbox Series S

Protótipo de desenvolvimento preparado a partir de um Mac. **Ainda não é um
minerador**: não recebe tarefas do pool, envia shares ou produz saldo.
Gera provas densas V3 completas para um teste offline, antes da integração
com uma tarefa real de mineração.

## O que está implementado

- BLAKE3 portátil em C, obtido do projeto oficial e fixado por versão e SHA-256.
- Derivação das sementes de ruído de provas Pearl V3 para matrizes densas.
- Geração de matrizes, ruído de consenso, árvores BLAKE3 e aberturas de linhas.
- Serialização canônica de `PlainProof` densa V3, rank 128 e blocos de 16×16.
- Referência de CPU para multiplicação exata de inteiros com sinal, redução XOR
  cumulativa a cada 128 termos e acumulação de 16 palavras com rotação de 13 bits.
- Shader HLSL equivalente, com entradas de 8 bits empacotadas e acumulador de
  32 bits; compilação antecipada para `cs_5_1`.
- Aplicativo UWP x64 separado do xllama, sem permissões de rede ou microfone.
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
rentabilidade ou estabilidade 24 horas. O shader prioriza correção e ainda
não foi otimizado.

## Trabalho que falta para minerar

1. Confirmar as provas completas produzidas no Xbox pelo verificador oficial.
2. Integrar tarefas e alvos atuais da Kryptex, respeitando `cert_version`,
   cancelamento de tarefas antigas e a interpretação correta dos inteiros.
3. Obter trabalho aceito pelo pool e medir a taxa efetiva.
4. Só então comparar receita e testar continuidade prolongada.

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
