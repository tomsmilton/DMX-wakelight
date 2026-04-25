"""
ESP32 + MAX485 + XLR5 DMX controller schematic.
MAX485 pin layout mirrors the real module:
    J1 header: RO, RE, DE, DI  (shown on the ESP-facing side, top-to-bottom)
    J2 header: VCC, B, A, GND  (shown on the XLR-facing side, top-to-bottom)
"""

import schemdraw
import schemdraw.elements as elm

with schemdraw.Drawing(show=False) as d:
    d.config(fontsize=11, unit=2)

    # --- ESP32 dev board ---
    # Pin order chosen so signal wires run roughly horizontally to their MAX485
    # counterparts, and power (3V3 top, GND bottom) is at the extremes for easy
    # routing over/under the MAX485 chip body.
    esp = elm.Ic(
        pins=[
            elm.IcPin(name='GND',    side='right', pin='1'),  # bottom
            elm.IcPin(name='GPIO17', side='right', pin='2'),  # TX -> DI
            elm.IcPin(name='GPIO4',  side='right', pin='3'),  # direction -> DE/RE
            elm.IcPin(name='GPIO16', side='right', pin='4'),  # RX -> RO
            elm.IcPin(name='3V3',    side='right', pin='5'),  # top
        ],
        w=4, h=8,
    ).label('ESP32 DevKit', loc='top', ofst=(0, 0.3))
    d += esp
    d += elm.Label().at((-2.3, 0)).label('USB', loc='left')

    # --- MAX485 module ---
    # Left side (ESP-facing), top-to-bottom: RO, RE, DE, DI  (J1 order)
    # Right side (XLR-facing), top-to-bottom: VCC, B, A, GND (J2 order)
    max485 = elm.Ic(
        pins=[
            elm.IcPin(name='DI',  side='left',  pin='1'),  # bottom-left
            elm.IcPin(name='DE',  side='left',  pin='2'),
            elm.IcPin(name='RE',  side='left',  pin='3'),
            elm.IcPin(name='RO',  side='left',  pin='4'),  # top-left
            elm.IcPin(name='GND', side='right', pin='5'),  # bottom-right
            elm.IcPin(name='A',   side='right', pin='6'),
            elm.IcPin(name='B',   side='right', pin='7'),
            elm.IcPin(name='VCC', side='right', pin='8'),  # top-right
        ],
        w=4, h=9,
    ).at((10, 0)).label('MAX485 module', loc='top', ofst=(0, 0.3))
    d += max485

    # --- XLR5 connector (fixture side) ---
    xlr = elm.Ic(
        pins=[
            elm.IcPin(name='1 (GND)',  side='left', pin='1'),
            elm.IcPin(name='2 (A/D-)', side='left', pin='2'),
            elm.IcPin(name='3 (B/D+)', side='left', pin='3'),
            elm.IcPin(name='4 (n/c)',  side='left', pin='4'),
            elm.IcPin(name='5 (n/c)',  side='left', pin='5'),
        ],
        w=4, h=8,
    ).at((19, 0)).label('XLR5 to fixture', loc='top', ofst=(0, 0.3))
    d += xlr

    # --- ESP32 -> MAX485 signals (direct wires on MAX485 left side) ---
    # GPIO17 (TX) -> DI
    d += elm.Line().at(esp.pin2).to(max485.pin1).color('blue')
    # GPIO16 (RX) -> RO
    d += elm.Line().at(esp.pin4).to(max485.pin4).color('green')
    # GPIO4 -> DE, with a bridge to RE (shorted on the module)
    d += elm.Line().at(esp.pin3).to(max485.pin2).color('orange')
    d += elm.Line().at(max485.pin2).to(max485.pin3).color('orange')
    d += elm.Dot().at(max485.pin2).color('orange')

    # --- ESP32 -> MAX485 power (right side of MAX485, routed around the body) ---
    # 3V3 -> VCC: route OVER the top of the MAX485.
    over_y = max485.pin8.y + 2.0
    step_x_top = esp.pin5.x + 0.5
    d += elm.Line().at(esp.pin5).to((step_x_top, esp.pin5.y)).color('red')
    d += elm.Line().endpoints((step_x_top, esp.pin5.y), (step_x_top, over_y)).color('red')
    d += elm.Line().endpoints((step_x_top, over_y), (max485.pin8.x, over_y)).color('red')
    d += elm.Line().endpoints((max485.pin8.x, over_y), max485.pin8).color('red')

    # GND -> MAX485 GND: route UNDER the bottom of the MAX485.
    under_y = max485.pin5.y - 2.0
    step_x_bot = esp.pin1.x + 0.5
    d += elm.Line().at(esp.pin1).to((step_x_bot, esp.pin1.y)).color('black')
    d += elm.Line().endpoints((step_x_bot, esp.pin1.y), (step_x_bot, under_y)).color('black')
    d += elm.Line().endpoints((step_x_bot, under_y), (max485.pin5.x, under_y)).color('black')
    d += elm.Line().endpoints((max485.pin5.x, under_y), max485.pin5).color('black')

    # --- MAX485 -> XLR wiring (right side of MAX485 to left side of XLR) ---
    # B -> XLR pin 3 (B/D+)
    d += elm.Line().at(max485.pin7).to(xlr.pin3).color('blue')
    # A -> XLR pin 2 (A/D-)
    d += elm.Line().at(max485.pin6).to(xlr.pin2).color('blue')
    # MAX485 GND -> XLR pin 1 (shared ground)
    d += elm.Line().at(max485.pin5).to(xlr.pin1).color('black')

    # --- XLR unused pins: short tick + 'n/c' label to the left of the connector ---
    nc_x = xlr.pin4.x - 0.8
    d += elm.Line().endpoints(xlr.pin4, (nc_x, xlr.pin4.y)).color('gray')
    d += elm.Label().at((nc_x, xlr.pin4.y)).label('n/c', loc='left', color='gray')
    d += elm.Line().endpoints(xlr.pin5, (nc_x, xlr.pin5.y)).color('gray')
    d += elm.Label().at((nc_x, xlr.pin5.y)).label('n/c', loc='left', color='gray')

    # --- Optional 120 Ω termination across A/B at the fixture ---
    term_x = xlr.pin2.x + 3.5
    d += elm.Line().endpoints(xlr.pin2, (term_x, xlr.pin2.y)).color('blue')
    d += elm.Line().endpoints(xlr.pin3, (term_x, xlr.pin3.y)).color('blue')
    d += elm.Resistor().endpoints(
        (term_x, xlr.pin2.y),
        (term_x, xlr.pin3.y),
    ).label('120Ω\n(term.)', loc='right')

    # --- Legend ---
    legend_y = under_y - 3
    entries = [
        ('red',    'Power (3.3V)'),
        ('black',  'Ground'),
        ('blue',   'Data (TX/A/B)'),
        ('green',  'Data (RX)'),
        ('orange', 'Direction (DE/RE)'),
    ]
    x = 0
    for color, text in entries:
        d += elm.Line().endpoints((x, legend_y), (x + 0.8, legend_y)).color(color)
        d += elm.Label().at((x + 1, legend_y)).label(text, loc='right')
        x += 4

    d.save('dmx_schematic.png', dpi=200)
    d.save('dmx_schematic.svg')

print("Saved: dmx_schematic.png and dmx_schematic.svg")
