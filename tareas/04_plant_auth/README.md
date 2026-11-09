# Tarea 4: quién puede mover el soplador (autorización por pid en `pneumatic_sim`)

`pneumatic_sim` es el "hardware" del ventilador: el soplador y las válvulas. Hasta ahora obedecía a cualquier
proceso que le mandara un `WRITE` o un `OVERRIDE`. La HMI, el proceso más expuesto (pantalla, red, comandos del
operador), tenía una conexión abierta con la planta para leer los sensores. Si alguien explota un bug de la HMI,
puede mandar "soplador al máximo, válvula espiratoria cerrada" y la planta obedece.

Compruébalo tú mismo con `scripts/b1_run.sh` (más abajo). Con el esqueleto, la HMI maliciosa (`src/evil_hmi.c`)
sube la presión de 20 a 41 cmH₂O. No llega a 60 solo porque `safety_mon` detecta presión alta y abre la válvula.

Ya cambié dos cosas:
- **La HMI ya no se conecta con la planta.** Lee sensores y signos vitales a través de `safety_mon`
  (`BG_MSG_MONITOR`). Esto quita el camino *normal*, pero no impide que un atacante abra el suyo: cualquier
  proceso puede hacer `name_open("bg_pneumatic")`.
- **`safety_mon` le dice a la planta quién es el controlador** (`BG_MSG_AUTH`), cada vez que lanza `breath_ctrl`.

Falta la parte central: que la planta **compruebe quién envía** cada mensaje. Esa es tu tarea, en
`plant_auth.c`: unas 25-35 líneas. Hasta que la completes, `auth_check()` acepta todo y el sistema funciona como
antes.

## Lo que necesitas saber: `struct _msg_info`

Cuando un servidor QNX recibe un mensaje, puede pedirle al kernel los datos del emisor:

```c
struct _msg_info info;
int rcvid = MsgReceive(chid, &m, sizeof m, &info);   /* el último argumento: el kernel llena info */
```

(Si ya tienes el `rcvid` y no pediste `info`, `MsgInfo(rcvid, &info)` da lo mismo.)

| Campo | Qué es |
|---|---|
| `pid` | **pid del proceso que envió el mensaje** |
| `tid` | hilo emisor dentro de ese proceso |
| `nd` | nodo de red (0 = esta máquina; QNX puede mandar mensajes entre máquinas con Qnet) |
| `chid`, `scoid`, `coid` | canal del servidor, id de la conexión del lado del servidor y del lado del cliente |
| `msglen` | bytes que se copiaron a tu buffer |
| `srcmsglen` | bytes que **mandó** el emisor (puede ser menos que `sizeof` de lo que esperas) |
| `dstmsglen` | tamaño del buffer de respuesta del emisor |
| `priority` | prioridad del hilo emisor |
| `type_id` | tipo de seguridad del emisor (secpol). En esta VM no hay política: es la capa 2, más adelante |

**La idea clave:** `info.pid` lo escribe el **kernel**, que sabe qué proceso hizo el `MsgSend`. El emisor no puede
cambiarlo. En cambio, todo lo que viene *dentro* del mensaje (`m`) lo escribió el emisor y puede ser mentira. Por
eso `bg_write_msg_t` no lleva un campo "soy breath_ctrl": no serviría de nada.

`bg_auth_msg_t` sí lleva un pid dentro (`auth->pid`), pero ese es el pid *que se quiere autorizar*. La confianza en
el mensaje no viene de ese campo, sino de quién lo envía (`info->pid`).

**Ojo con `srcmsglen`:** la planta recibe en un `union` del tamaño del mensaje más grande. Si un emisor manda
solo los 4 bytes de la cabecera con `type = BG_MSG_AUTH`, el campo `pid` del union contiene **basura del mensaje
anterior**. La planta debe comprobar que llegó el mensaje completo.

Para el uid/gid del emisor existe `ConnectClientInfo()`. Aquí todo corre como root, así que el uid no distingue
a nadie: por eso usamos el pid.

## Reglas

| Mensaje | Se acepta si... |
|---|---|
| `READ`, `VITALS`, `PATIENT`, `FAULT`, tipos desconocidos | siempre (no mueven actuadores; un tipo desconocido lo contesta la planta con `ENOSYS`) |
| `AUTH` | el mensaje está completo, `auth->pid > 0` y el emisor es el **dueño**. Si todavía no hay dueño, el emisor pasa a serlo. Al aceptarlo: `ctrl_pid = auth->pid` |
| `WRITE` | hay controlador autorizado y el emisor **es** `ctrl_pid` |
| `OVERRIDE` | hay dueño y el emisor **es** `safety_pid` |

El orden de `AUTH` importa: un `AUTH` inválido (truncado o con pid ≤ 0) no debe convertir a nadie en dueño.

`FAULT` se acepta de cualquiera porque simula el mundo físico (un tubo que se desconecta); un ventilador real no
tendría ese mensaje. En la capa 2 lo limitaremos al tipo de `bg_fault`.

**Lo que ya está hecho** (no lo toques): `pneumatic_sim` llama a `auth_check()` con cada mensaje. Si devuelve 0,
responde `MsgError(rcvid, EPERM)`, no toca los actuadores y cuenta el rechazo. `safety_mon` lo lee cada 100 ms,
registra el evento `COMANDO_NO_AUTORIZADO` y activa la alarma `SEGURIDAD` durante 10 s.

### El primero que llega es el dueño: ¿no es un hueco?

Sí, y conviene saberlo explicarlo. Se llama *trust on first use*. Aquí funciona porque `safety_mon` arranca
justo después de la planta y es el que lanza la HMI: cuando la HMI existe, la planta ya tiene dueño.

Una carrera parecida había con `breath_ctrl`. Si `safety_mon` lo lanza y *después* lo autoriza, el controlador
podría mandar su primer `WRITE` antes de estar autorizado. Lo resolví con una extensión de QNX:
`posix_spawnattr_setxflags(&attr, POSIX_SPAWN_HOLD)`. El hijo se crea detenido, antes de su primera instrucción;
`safety_mon` lo autoriza y lo suelta con `SIGCONT`. Mira `spawn_ctrl()` en `src/safety_mon.c`.

La capa 2 (política secpol) cierra el hueco del dueño: el kernel no dejará que un proceso del tipo de la HMI ni
siquiera se *conecte* a la planta.

## Antes de programar
Responde con tus palabras y mándamelo junto con el código:
1. ¿Por qué no basta con que la HMI "no use" su conexión con la planta, y hace falta que la planta compruebe al
   emisor? ¿Qué cambia entre un bug y un ataque?
2. Cuando `safety_mon` relanza `breath_ctrl`, ¿qué pasa con un `WRITE` del controlador *viejo* si siguiera vivo
   (por ejemplo, congelado con `SIGSTOP` y luego reanudado)? ¿Por qué es importante?
3. Los pid se reutilizan cuando un proceso muere. ¿En qué caso podría eso engañar a esta comprobación? ¿Qué
   haría falta para que fuera un problema real?

## Cómo probarlo

**1. Prueba unitaria** (en la VM, porque necesita `struct _msg_info` de QNX):
```bash
make tarea4
```
Con el esqueleto **debe fallar** (11 casos). Cuando quede bien, termina con `Todo bien`.

**2. Prueba de integración: la HMI maliciosa** (40 s en la VM; la VM debe estar encendida):
```bash
scripts/b1_run.sh mi_prueba
```
Lanza el ventilador completo con la HMI; a los 20 s `evil_hmi` ataca durante 2 s. `scripts/b1_check.py`
comprueba cuatro cosas:
1. la planta rechaza los tres ataques (`AUTH` para sí misma, `OVERRIDE` y cada `WRITE`) con `EPERM`;
2. la presión durante el ataque no supera la de las respiraciones anteriores (+1.5 cmH₂O);
3. `safety_mon` registra `COMANDO_NO_AUTORIZADO`;
4. la HMI sigue mostrando datos (ahora llegan por `safety_mon`).

Con el esqueleto da `PRUEBA B1: FALLA` (la presión sube a ~41 cmH₂O). Con tu código debe dar `PRUEBA B1: PASA`.
Revisa también que el ventilador siga respirando con normalidad: si rechazas por error a `breath_ctrl`, el
controlador muere en su primer `WRITE` y verás fallas del controlador en el log.

No modifiques `plant_auth.h` ni `test_plant_auth.c`.
