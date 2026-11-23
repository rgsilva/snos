# discordbridge

Liga os canais de texto de um servidor do Discord ao cliente IRC do SNOS (`apps/irc/irc.c`).
Cada canal do Discord vira um canal IRC. O console entra nos que quiser, cada um em uma janela.
O programa e um bot do Discord e, ao mesmo tempo, um servidor IRC minimo
que atende um unico cliente: o console.

```
servidor do Discord  <->  discordbridge (notebook)  <->  console: irc 192.168.x.x:6667 snes #palestra
```

O servidor e o que contem o canal de `CHANNEL_ID`. Esse e o canal padrao.
A lista de nomes de cada canal mostra as pessoas de verdade que podem ver o canal.

Python 3.12. Unica dependencia: `discord.py`.

## 1. Criar o bot

Documentacao: https://discordpy.readthedocs.io/en/stable/discord.html e
https://docs.discord.com/developers/topics/gateway

1. Abra https://discord.com/developers/applications e clique em **New Application**.
2. Aba **Bot**. Clique em **Reset Token** e copie o token. Ele e uma senha. Nao mostre no telao.
3. Na mesma aba, em **Privileged Gateway Intents**, ligue dois itens:
   - **Message Content Intent**. Sem ele o bot ve mensagens vazias e o programa nao funciona.
   - **Server Members Intent**. Sem ele a lista de nomes so tem quem falou desde que o programa abriu
     (o log avisa com uma linha). Com ele a lista tem todo mundo que ve o canal.

   (Ate 100 servidores nao precisa de verificacao.)
4. Aba **OAuth2 > URL Generator**: marque o scope `bot`.
   Permissoes minimas: **View Channels**, **Send Messages**, **Read Message History**.
   Elas somam 68608, entao o convite fica assim (troque `APP_ID` pelo Application ID da aba General Information):

   `https://discord.com/oauth2/authorize?client_id=APP_ID&scope=bot&permissions=68608`

   Referencia dos valores: https://docs.discord.com/developers/topics/permissions
5. Abra o convite, escolha o servidor e autorize. Voce precisa de **Manage Server** la.
6. Veja se os canais que voce quer mostrar nao negam essas permissoes ao bot (permissoes do canal).
   O bot so expoe canais onde ele pode ver **e** escrever.
7. Id do canal padrao: Discord > Configuracoes > Avancado > ligue **Modo Desenvolvedor**.
   Clique com o botao direito no canal e escolha **Copiar ID do canal**.
   (https://support.discord.com/hc/en-us/articles/206346498)

## 2. Instalar e rodar

```sh
cd snes/tools/discordbridge
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

Configuracao por variaveis de ambiente, ou um arquivo `discordbridge.env`
(`CHAVE=valor`, uma por linha; ja esta no .gitignore). O token pode ficar em `token.txt`.

| Variavel (prefixo `DISCORDBRIDGE_`) | Padrao | Para que serve |
|---|---|---|
| `TOKEN` ou arquivo `token.txt` | (obrigatorio) | token do bot |
| `CHANNEL_ID` | (obrigatorio) | id do canal padrao; define tambem o servidor |
| `IRC_CHANNEL` | (vazio) | nome IRC do canal padrao, por exemplo `#palestra` (ate 24 letras). Vazio: usa o nome do canal |
| `ALLOW_CHANNELS` | (vazio) | ids de canais separados por virgula; so esses aparecem. Vazio: todos |
| `BIND`, `PORT` | `0.0.0.0`, `6667` | onde o servidor IRC escuta |
| `MAX_CHARS` | `96` | tamanho maximo do texto |
| `NICK_MAX` | `15` | tamanho maximo dos nicks (de 3 a 16; o cliente guarda 16) |
| `INTERVAL` | `1.5` | segundos entre linhas para o console (vale para todos os canais juntos) |
| `QUEUE` | `20` | fila maxima; estourou, perde a mais antiga |
| `NAMES_MAX` | `48` | nomes por canal, contando o console (o cliente guarda 48) |
| `NAMES_INTERVAL` | `3` | segundos minimos entre duas listas novas de um canal |
| `NAMES_ORDER_INTERVAL` | `60` | idem, quando so a ordem mudou |
| `MUTE_FILE` | `MUTE` | se o arquivo existe, nada passa |
| `BLOCKLIST` | `blocklist.txt` | lista de palavras (opcional) |
| `ALLOW_ROLE` | (vazio) | id de cargo; so quem tem passa |
| `PREFIX` | `**[SNES]**` | prefixo no Discord |

Exemplo de `discordbridge.env`:

```
DISCORDBRIDGE_TOKEN=cole-o-token-aqui
DISCORDBRIDGE_CHANNEL_ID=123456789012345678
```

Rodar:

```sh
.venv/bin/python discordbridge.py
```

O log mostra uma linha `bridged channels: #palestra=palestra-ao-vivo, #geral=geral, ...`.
Ela diz qual canal IRC corresponde a qual canal do Discord.

Ctrl-C para sair. Firewall: libere a porta TCP 6667 na rede local
(`sudo ufw allow from 192.168.0.0/16 to any port 6667 proto tcp`, ou a regra do seu firewall).
Descubra o IP do notebook com `ip -4 addr`.

## 3. No console

```
irc 192.168.x.x:6667 snes #palestra
```

Formato do programa: `irc HOST[:PORT] [NICK] [#CANAL]`.
O nick padrao e `snes`; o console entra no canal informado sozinho depois do boas-vindas.
O canal da linha de comando pode ser o `IRC_CHANNEL` ou o nome derivado do canal padrao.
Se o canal for omitido, digite `/join #palestra`.

O boas-vindas diz qual e o canal padrao. Depois use:

- `/raw LIST` mostra todos os canais, um por linha, na janela de status (a primeira):
  `#geral (4) Conversa solta`. O numero e quantas pessoas veem o canal.
  O cliente nao tem um comando `/list`; `/raw LIST` e o jeito.
- `/join #geral` entra em outro canal. Abre uma janela nova. Pode entrar em varios.
- `/part` sai do canal da janela. `/names` pede a lista de nomes de novo. `^U` mostra os nomes.
- `^N` e `^P` trocam de janela. `TAB` lista as janelas.

Como sao os nomes dos canais: `#` mais o nome do Discord, em minusculas, so letras, numeros, `-` e `_`,
ate 23 letras. Acentos perdem o acento. Se dois canais ficarem iguais, o de id menor fica com o nome
e os outros ganham um numero (`#chat`, `#chat2`).

Mensagens do Discord so chegam nos canais em que o console entrou.
Escrever em um canal onde o console nao entrou da erro 404 na janela de status.
Entrar em um nome que nao existe da erro 403.

**Renomear um canal.** Se o canal for renomeado no Discord com o console dentro,
o servidor manda um aviso (NOTICE) na janela de status e o console continua com o nome antigo
ate dar `/part`. Depois do `/part`, vale o nome novo.
**Apagar um canal** (ou tirar a permissao do bot) tira o console do canal com um KICK.

**Lista de nomes.** O primeiro nome e o do console. Depois vem quem falou por ultimo,
depois os outros em ordem alfabetica. So entram pessoas, nao bots.
Cada pessoa tem sempre o mesmo nick, e e o mesmo nick que aparece nas mensagens dela.
Se duas pessoas ficarem com o mesmo nick, a segunda ganha um numero (`ana`, `ana2`).
O servidor manda uma lista nova sozinho quando alguem entra ou sai do servidor, muda de nome,
ganha ou perde acesso ao canal ou fala pela primeira vez. No maximo uma por canal a cada 3 s.
Se mudou so a ordem, no maximo uma por minuto.
Isso nao imprime linhas de entrada e saida no console. O cliente escreve so `* N users here`.
Hoje o cliente soma a lista nova a antiga em vez de trocar; isso e um defeito do cliente.

## 4. Ensaio sem console

Sem Discord e sem token:

```sh
.venv/bin/python discordbridge.py --dry-run
```

O programa cria um servidor falso. O padrao tem quatro canais (`palestra`, `geral`, `off-topic`,
`so-staff`) e cinco pessoas. O canal padrao e o `palestra`.
Para usar o seu, escreva um arquivo e passe `--fake-guild arquivo.txt`:

```
# channel <id> <nome> [| topico]
# member <id> <nome de exibicao> [| ids dos canais que a pessoa ve; vazio: todos]
channel 7 palestra | Canal da palestra
channel 8 geral
member 1 Ana
member 2 Joao Silva | 7,8
```

Digite no terminal. Uma linha `nome: texto` vale como mensagem no canal padrao.
`#geral nome: texto` vale como mensagem no canal `geral`
(pode ser o nome do Discord ou o nome IRC). Quem ainda nao existe entra no servidor.
Comandos que mudam o servidor falso:

| Comando | Efeito |
|---|---|
| `/join NOME`, `/leave NOME` | uma pessoa entra no servidor, ou sai |
| `/rename NOME > NOVO` | a pessoa muda de nome de exibicao |
| `/newchan NOME [\| topico]` | cria um canal |
| `/delchan NOME` | apaga um canal |
| `/renchan NOME > NOVO` | renomeia um canal |
| `/hide NOME #canal`, `/show NOME #canal` | tira a pessoa do canal, ou poe de volta |
| `/nomembers`, `/members` | simula a falta do Server Members Intent, ou a volta dele |
| `/help` | lista os comandos |

O que iria para o Discord aparece como `[dry-run] would post to Discord #canal: ...`.
O programa continua rodando se o stdin acabar; use Ctrl-C.

Teste real com um cliente IRC de desktop (HexChat, irssi) em `127.0.0.1:6667`, canal `#palestra`.
`LIST` mostra os canais.
Ou com `nc`:

```sh
nc 127.0.0.1 6667
NICK snes
USER snes 0 * :teste
JOIN #palestra
PRIVMSG #palestra :ola do nc
```

(Termine cada linha com Enter. `nc` manda so `\n`, e o servidor aceita.)

Testes automaticos (uns 6 segundos, so localhost, sem Discord; Python 3.12):

```sh
python3 -m unittest discover -s tests -v
```

Com o `discord.py` instalado, o mesmo comando confere tambem se os nomes usados do
`discord.py` existem (`tests/test_discord_api.py`).

## 5. No palco

**Quais canais mostrar.** Mostre so os canais da palestra, de preferencia um ou dois.
O bot so ve o que o cargo dele ve. O jeito mais limpo de esconder um canal e tirar dele a
permissao **View Channel** do cargo do bot (ou do `@everyone`). Canal que o bot nao ve nao aparece.
Canal onde ele ve mas nao escreve tambem nao aparece.
Alternativa: `ALLOW_CHANNELS` com os ids dos canais permitidos. Mas o bot continua vendo os outros,
entao confie mais nas permissoes. Canais de staff nao devem ser visiveis ao bot.

**Canais.** Ligue o modo lento (Slow Mode) de 5 a 10 segundos em cada canal mostrado.
Ligue o AutoMod com uma lista de palavras. Deixe so o bot e a plateia nos canais.

**Blocklist, mudo e fila valem para todos os canais.** Existe uma fila so, porque o console e um so.
Com muitos canais ativos, a fila enche e as mensagens mais antigas caem.
Por isso entre so nos canais que importam.

**Blocklist.** Copie `blocklist.example.txt` para `blocklist.txt` e edite.
Uma palavra ou frase por linha. O arquivo e relido sozinho quando muda.
Bateu na palavra: a mensagem some do console e aparece no log.

**Mudo.** `touch MUTE` corta tudo, nos dois sentidos, em ate um intervalo (1,5 s).
`rm MUTE` volta. O que estava na fila e descartado.

**Tela de 32 colunas.** O texto padrao e de 96 letras (tres linhas de tela). Mude com `MAX_CHARS`.

**Se o WiFi cair.** O console reconecta com o mesmo comando `irc ...`.
A conexao nova substitui a velha, mesmo que a velha ainda pareca viva.
Mensagens que chegaram sem console sao perdidas, nao guardadas.
Se o IP do notebook mudou, use o novo.

**Linhas do log.**

| Linha | Significa |
|---|---|
| `Discord ready: server ...` | bot conectado e viu o servidor |
| `bridged channels: #a=nome, ...` | quais canais IRC existem e de qual canal do Discord vem cada um |
| `Server Members Intent is off: ...` | falta ligar o intent no portal; a lista de nomes so tem quem falou |
| `IRC server listening on ...` | servidor IRC no ar |
| `console ... connected` / `disconnected` | console entrou / saiu (com o endereco) |
| `console ... joined #canal` | console esta no canal; agora as mensagens desse canal passam |
| `D->C #canal nick: texto` | mensagem do Discord enviada ao console |
| `C->D #canal ...` | mensagem do console enviada ao Discord |
| `dropped N messages from channels the console has not joined` | uma vez por minuto, se houve; nao ha linha por mensagem |
| `drop (no console)` | ninguem no canal IRC; mensagem descartada |
| `drop (muted)` | arquivo MUTE existe |
| `drop (queue full)` | fila cheia; a mais antiga caiu |
| `drop (blocklist (...))` | bateu na lista |
| `drop (role)` | autor sem o cargo permitido |
| `drop (empty)` | sobrou nada depois da limpeza (so emoji, por exemplo) |
| `drop (discord backlog)` | Discord lento; mensagem do console descartada |
| `discord send failed` | o Discord recusou o envio; o IRC continua normal |

Bots e canais onde o console nao entrou nao entram no log (use `-v` para ver).

**Checklist de 10 minutos antes.**

- [ ] Notebook e console na mesma rede; IP anotado.
- [ ] `discordbridge.py` rodando; log mostra `Discord ready` e `bridged channels` com so os canais certos.
- [ ] `/raw LIST` no console mostra so esses canais.
- [ ] Mande uma mensagem pelo celular: ela chega no console.
- [ ] Digite no console: aparece no Discord com `[SNES]`.
- [ ] Modo lento e AutoMod ligados.
- [ ] `blocklist.txt` revisada.
- [ ] `touch MUTE` e `rm MUTE` testados.
- [ ] Token fora da tela. Bateria e carregador.
- [ ] Hotspot do celular como plano B.

## 6. Limites conhecidos

**O que passa e o que nao passa.**

- Passam: canais de texto onde o bot ve e escreve.
- Nao passam: canais de voz, palco, categorias, foruns, threads (topicos) e canais de anuncio.
  Mensagens dentro de threads nao passam.
- Canais criados, renomeados ou apagados, e mudancas de permissao, entram sozinhos (em cerca de 1 s).

**O resto.**

- O console mostra so ASCII imprimivel. Acentos viram a letra base (`ação` vira `acao`). Emojis somem.
- Um cliente por vez.
- Entradas e saidas de pessoas no Discord nao aparecem como linhas no console; so a lista de nomes muda.
  Edicoes e remocoes de mensagens nao passam.
  Resposta (reply) chega so como texto. Anexo vira `[imagem]` ou `[anexo]`, figurinha `[figurinha]`, link `[link]`.
- O cliente `irc` do SNOS nao tem `/me` nem `/list`. O servidor entende CTCP ACTION (vira `*texto*` no Discord),
  mas no console so da para mandar via `/raw` com o byte 0x01, o que e impratico.
- A resposta do `LIST` aparece na janela de status, nao na do canal.
- O cliente guarda 16 letras por nick e 48 nicks por canal. Servidores grandes mostram so os 48 primeiros da ordem acima.
- Presenca (online ou offline) nao existe: o intent de presenca esta desligado. A lista tem todos que veem o canal.
- Mensagem para uma pessoa (`/msg`) nao passa; o servidor responde com um NOTICE na janela de status.
- Nao ha TLS nem senha no servidor IRC. Use so em rede de confianca.
- O texto do console pode conter markdown do Discord; mencoes sao neutralizadas.
