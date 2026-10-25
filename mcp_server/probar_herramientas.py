"""
Prueba automática del servidor MCP: lo lanza igual que Claude Desktop (stdio),
lista sus herramientas y llama a cada una, en el mismo orden que las frases
del enunciado. Requiere el ESP32 o simulador_objeto.py en ejecución.

    python probar_herramientas.py
"""

import asyncio
import os
import sys
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

SERVIDOR = Path(__file__).with_name("server.py")
ESPERA_CONEXION_S = 30  # La primera conexión al broker público puede tardar varios segundos.

# (frase del usuario, herramienta que debería elegir Claude, argumentos)
CASOS = [
    ("¿Está conectado el objeto?", "estado_conexion", {}),
    ("¿A qué distancia está el objeto ahora?", "obtener_distancia", {}),
    ("¿El objeto se está acercando?", "obtener_historial", {"cantidad": 8}),
    ("Enciende el LED verde.", "controlar_led", {"color": "verde", "estado": "on"}),
    ("Enciende el LED rojo y apaga los demás.", "establecer_leds",
     {"rojo": "on", "amarillo": "off", "verde": "off"}),
    ("¿Qué LEDs están encendidos?", "obtener_estado_leds", {}),
    ("Apaga todos los LEDs.", "apagar_todos", {}),
    ("¿Qué LEDs están encendidos?", "obtener_estado_leds", {}),
]


async def main() -> None:
    parametros = StdioServerParameters(
        command=sys.executable, args=[str(SERVIDOR)], env=dict(os.environ)
    )
    async with stdio_client(parametros) as (lectura, escritura):
        async with ClientSession(lectura, escritura) as sesion:
            await sesion.initialize()
            herramientas = (await sesion.list_tools()).tools
            print("Herramientas:", ", ".join(h.name for h in herramientas))
            for _ in range(ESPERA_CONEXION_S):
                estado = await sesion.call_tool("estado_conexion", {})
                if '"broker_conectado": true' in estado.content[0].text:
                    break
                await asyncio.sleep(1)
            await asyncio.sleep(1)  # Recibe los mensajes retenidos de /conexion y /estado.

            for frase, herramienta, argumentos in CASOS:
                resultado = await sesion.call_tool(herramienta, argumentos)
                texto = " ".join(resultado.content[0].text.split())
                print(f"\n> {frase}\n  {herramienta}({argumentos}) -> {texto}")
                await asyncio.sleep(1)  # Da tiempo a que el objeto publique su nuevo estado.


if __name__ == "__main__":
    asyncio.run(main())
