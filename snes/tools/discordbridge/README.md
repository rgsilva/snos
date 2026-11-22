# discordbridge

Liga UM canal do Discord ao cliente IRC do SNOS (`apps/irc/irc.c`).
O programa e um bot do Discord e, ao mesmo tempo, um servidor IRC minimo
que atende um unico cliente: o console.

```
canal do Discord  <->  discordbridge (notebook)  <->  console: irc 192.168.x.x:6667 snes #palestra
```

Python 3.12. Unica dependencia: `discord.py`.

## 1. Criar o bot

Documentacao: https://discordpy.readthedocs.io/en/stable/discord.html e
https://docs.discord.com/developers/topics/gateway

1. Abra https://discord.com/developers/applications e clique em **New Application**.
2. Aba **Bot**. Clique em **Reset Token** e copie o token. Ele e uma senha. Nao mostre no telao.
3. Na mesma aba, em **Privileged Gateway Intents**, ligue **Message Content Intent**.
   Sem isso o bot ve mensagens vazias e o programa nao funciona.
   (Ate 100 servidores nao precisa de verificacao.)
4. Aba **OAuth2 > URL Generator**: marque o scope `bot`.
   Permissoes minimas: **View Channels**, **Send Messages**, **Read Message History**.
   Elas somam 68608, entao o convite fica assim (troque `APP_ID` pelo Application ID da aba General Information):

   `https://discord.com/oauth2/authorize?client_id=APP_ID&scope=bot&permissions=68608`

   Referencia dos valores: https://docs.discord.com/developers/topics/permissions
5. Abra o convite, escolha o servidor e autorize. Voce precisa de **Manage Server** la.
6. Veja se o canal da palestra nao nega essas permissoes ao bot (permissoes do canal).
7. Id do canal: Discord > Configuracoes > Avancado > ligue **Modo Desenvolvedor**.
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
| `CHANNEL_ID` | (obrigatorio) | id do canal do Discord |
| `IRC_CHANNEL` | `#palestra` | canal IRC (ate 24 letras) |
| `BIND`, `PORT` | `0.0.0.0`, `6667` | onde o servidor IRC escuta |
| `MAX_CHARS` | `160` | tamanho maximo do texto |
| `INTERVAL` | `1.5` | segundos entre linhas para o console |
| `QUEUE` | `20` | fila maxima; estourou, perde a mais antiga |
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

Ctrl-C para sair. Firewall: libere a porta TCP 6667 na rede local
(`sudo ufw allow from 192.168.0.0/16 to any port 6667 proto tcp`, ou a regra do seu firewall).
Descubra o IP do notebook com `ip -4 addr`.

## 3. No console

```
irc 192.168.x.x:6667 snes #palestra
```

Formato do programa: `irc HOST[:PORT] [NICK] [#CANAL]`. O canal tem de ser o mesmo de `IRC_CHANNEL`.
O nick padrao e `snes`; o console entra no canal sozinho depois do boas-vindas.
Se o canal for omitido, digite `/join #palestra`.
Qualquer outro canal e recusado.

## 4. Ensaio sem console

Sem Discord e sem token:

```sh
.venv/bin/python discordbridge.py --dry-run
```

Digite linhas `nome: texto`. Elas valem como mensagens do Discord.
O que iria para o Discord aparece como `[dry-run] would post to Discord: ...`.
O programa continua rodando se o stdin acabar; use Ctrl-C.

Teste real com um cliente IRC de desktop (HexChat, irssi) em `127.0.0.1:6667`, canal `#palestra`.
Ou com `nc`:

```sh
nc 127.0.0.1 6667
NICK snes
USER snes 0 * :teste
JOIN #palestra
PRIVMSG #palestra :ola do nc
```

(Termine cada linha com Enter. `nc` manda so `\n`, e o servidor aceita.)

Testes automaticos (uns 3 segundos, sem rede):

```sh
python3 -m unittest discover -s tests -v
```

## 5. No palco

**Canal do Discord.** Ligue o modo lento (Slow Mode) de 5 a 10 segundos.
Ligue o AutoMod com uma lista de palavras. Deixe so o bot e a plateia no canal.

**Blocklist.** Copie `blocklist.example.txt` para `blocklist.txt` e edite.
Uma palavra ou frase por linha. O arquivo e relido sozinho quando muda.
Bateu na palavra: a mensagem some do console e aparece no log.

**Mudo.** `touch MUTE` corta tudo, nos dois sentidos, em ate um intervalo (1,5 s).
`rm MUTE` volta. O que estava na fila e descartado.

**Se o WiFi cair.** O console reconecta com o mesmo comando `irc ...`.
A conexao nova substitui a velha, mesmo que a velha ainda pareca viva.
Mensagens que chegaram sem console sao perdidas, nao guardadas.
Se o IP do notebook mudou, use o novo.

**Linhas do log.**

| Linha | Significa |
|---|---|
| `Discord ready: server ..., channel ...` | bot conectado e viu o canal |
| `IRC server listening on ...` | servidor IRC no ar |
| `console ... connected` / `disconnected` | console entrou / saiu (com o endereco) |
| `console ... joined #palestra` | console esta no canal; agora as mensagens passam |
| `D->C nick: texto` | mensagem do Discord enviada ao console |
| `C->D ...` | mensagem do console enviada ao Discord |
| `drop (no console)` | ninguem no canal IRC; mensagem descartada |
| `drop (muted)` | arquivo MUTE existe |
| `drop (queue full)` | fila cheia; a mais antiga caiu |
| `drop (blocklist (...))` | bateu na lista |
| `drop (role)` | autor sem o cargo permitido |
| `drop (empty)` | sobrou nada depois da limpeza (so emoji, por exemplo) |
| `drop (discord backlog)` | Discord lento; mensagem do console descartada |
| `discord send failed` | o Discord recusou o envio; o IRC continua normal |

Bots e outros canais nao entram no log (use `-v` para ver).

**Checklist de 10 minutos antes.**

- [ ] Notebook e console na mesma rede; IP anotado.
- [ ] `discordbridge.py` rodando; log mostra `Discord ready`.
- [ ] Mande uma mensagem pelo celular: ela chega no console.
- [ ] Digite no console: aparece no Discord com `[SNES]`.
- [ ] Modo lento e AutoMod ligados.
- [ ] `blocklist.txt` revisada.
- [ ] `touch MUTE` e `rm MUTE` testados.
- [ ] Token fora da tela. Bateria e carregador.
- [ ] Hotspot do celular como plano B.

## 6. Limites conhecidos

- O console mostra so ASCII imprimivel. Acentos viram a letra base (`ação` vira `acao`). Emojis somem.
- Um cliente por vez; so um canal do Discord e um canal IRC.
- Entradas e saidas do Discord nao aparecem no console. Edicoes e remocoes nao passam.
  Resposta (reply) chega so como texto. Anexo vira `[imagem]` ou `[anexo]`, figurinha `[figurinha]`, link `[link]`.
- O cliente `irc` do SNOS nao tem `/me`. O servidor entende CTCP ACTION (vira `*texto*` no Discord), mas no console so da para mandar via `/raw` com o byte 0x01, o que e impratico.
- Mensagem para uma pessoa (`/msg`) nao passa; o servidor responde com um NOTICE na janela de status.
- A lista de usuarios do console tem so voce e `discord`.
- Nao ha TLS nem senha no servidor IRC. Use so em rede de confianca.
- O texto do console pode conter markdown do Discord; mencoes sao neutralizadas.
