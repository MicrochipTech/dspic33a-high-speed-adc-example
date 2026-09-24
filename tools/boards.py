"""
boards.py - the two evaluation kits this example runs on, and where an
ADC channel comes out on each of them.

GENERATED, do not hand-edit.

EV74H48A: dsPIC33 Curiosity Platform Development Board with the
dsPIC33AK512MPS512 GP DIM. The DIM information sheet DS70005563A says
which device pin each DIM pin carries and what the base board wires it
to; that is where "mikroBUS A pin 1" and "potentiometer" below come from.
Its rows appear twice (by DIM pin and by device pin) and their net names
carry the DIM pin as a prefix, which is what resolved the direction.
mikroBUS pin numbers follow the click board standard (AN = 1, RST = 2,
CS = 3, SCK = 4, MISO = 5, MOSI = 6, SDA = 11, SCL = 12, TX = 13,
RX = 14, INT = 15, PWM = 16); pins 7..10 are power and ground and do not
reach the device.

EV17P63A: dsPIC33AK512MPS506 Curiosity Nano. Its user guide DS70005634A
puts the whole pinout in one figure; the pad order below is that figure's
own, read from the USB end, with the device pins from pins64.py.

What is NOT here, because neither document states it: pins the boards
leave unconnected. A channel that resolves to no entry is reported as
"not brought out on this board", not guessed.
"""

# mikroBUS signal name -> socket pin, the click board standard.
MIKROBUS_PINS = {"AN": 1, "RST": 2, "CS": 3, "SCK": 4, "MISO": 5, "MOSI": 6,
                 "+3.3V": 7, "GND": 8, "GND2": 9, "+5V": 10, "SDA": 11,
                 "SCL": 12, "TX": 13, "RX": 14, "INT": 15, "PWM": 16}

BOARDS = {
    "EV74H48A": {
        "title": "EV74H48A \u00b7 dsPIC33 Curiosity Platform + MPS512 DIM",
        "device": "dsPIC33AK512MPS512",
        "package": "TQFP-128",
        "pin_count": 128,
        "default_core": 3,
        "default_pinsel": 5,
        # name -> {socket pin: {"dev": device pin, "sig": signal name}}
        "connectors": {
            'XPRO1': {
                1: {
                    'dev': 67,
                    'dim': 15,
                    'sig': '',
                },
                3: {
                    'dev': 45,
                    'dim': 13,
                    'sig': '',
                },
                4: {
                    'dev': 46,
                    'dim': 11,
                    'sig': '',
                },
                5: {
                    'dev': 47,
                    'dim': 9,
                    'sig': '',
                },
                6: {
                    'dev': 55,
                    'dim': 7,
                    'sig': '',
                },
                7: {
                    'dev': 57,
                    'dim': 5,
                    'sig': '',
                },
                8: {
                    'dev': 58,
                    'dim': 3,
                    'sig': '',
                },
                9: {
                    'dev': 60,
                    'dim': 1,
                    'sig': '',
                },
                10: {
                    'dev': 61,
                    'dim': 2,
                    'sig': '',
                },
                15: {
                    'dev': 43,
                    'dim': 12,
                    'sig': '',
                },
                16: {
                    'dev': 41,
                    'dim': 14,
                    'sig': '',
                },
                17: {
                    'dev': 40,
                    'dim': 16,
                    'sig': '',
                },
                18: {
                    'dev': 34,
                    'dim': 18,
                    'sig': '',
                },
            },
            'XPRO2': {
                3: {
                    'dev': 33,
                    'dim': 20,
                    'sig': '',
                },
                4: {
                    'dev': 31,
                    'dim': 22,
                    'sig': '',
                },
                5: {
                    'dev': 29,
                    'dim': 24,
                    'sig': '',
                },
                6: {
                    'dev': 28,
                    'dim': 26,
                    'sig': '',
                },
            },
            'mikroBUS A': {
                1: {
                    'dev': 4,
                    'dim': 77,
                    'sig': 'AN',
                },
                2: {
                    'dev': 20,
                    'dim': 79,
                    'sig': 'RST',
                },
                3: {
                    'dev': 26,
                    'dim': 81,
                    'sig': 'CS',
                },
                4: {
                    'dev': 30,
                    'dim': 83,
                    'sig': 'SCK',
                },
                5: {
                    'dev': 89,
                    'dim': 85,
                    'sig': 'MISO',
                },
                6: {
                    'dev': 90,
                    'dim': 87,
                    'sig': 'MOSI',
                },
                13: {
                    'dev': 17,
                    'dim': 69,
                    'sig': 'TX',
                },
                14: {
                    'dev': 15,
                    'dim': 71,
                    'sig': 'RX',
                },
                15: {
                    'dev': 11,
                    'dim': 73,
                    'sig': 'INT',
                },
                16: {
                    'dev': 84,
                    'dim': 75,
                    'sig': 'PWM',
                },
            },
            'mikroBUS B': {
                1: {
                    'dev': 27,
                    'dim': 29,
                    'sig': 'AN',
                },
                2: {
                    'dev': 74,
                    'dim': 31,
                    'sig': 'RST',
                },
                3: {
                    'dev': 35,
                    'dim': 33,
                    'sig': 'CS',
                },
                4: {
                    'dev': 36,
                    'dim': 35,
                    'sig': 'SCK',
                },
                5: {
                    'dev': 39,
                    'dim': 37,
                    'sig': 'MISO',
                },
                6: {
                    'dev': 42,
                    'dim': 39,
                    'sig': 'MOSI',
                },
                13: {
                    'dev': 62,
                    'dim': 21,
                    'sig': 'TX',
                },
                14: {
                    'dev': 65,
                    'dim': 23,
                    'sig': 'RX',
                },
                15: {
                    'dev': 49,
                    'dim': 25,
                    'sig': 'INT',
                },
                16: {
                    'dev': 66,
                    'dim': 27,
                    'sig': 'PWM',
                },
            },
        },
        # device pin -> what sits on it on the board
        "onboard": {
            5: 'potentiometer',
            7: 'capacitive touch pad 1',
            13: 'capacitive touch pad 1R',
            14: 'capacitive touch pad 2',
            16: 'capacitive touch pad 2R',
            18: 'capacitive touch pad 3',
            19: 'capacitive touch pad DS',
            25: 'capacitive touch pad 3R',
            44: 'push button 3',
            48: 'push button 2',
            63: 'push button 1',
            71: 'user LED 0',
            72: 'user LED 1',
            87: 'user LED 2',
            88: 'user LED 3',
            91: 'user LED 4',
            92: 'user LED 5',
            95: 'user LED 6',
            96: 'user LED 7',
            98: 'user LED R',
            101: 'user LED G',
            106: 'user LED B',
        },
    },
    "EV17P63A": {
        "title": "EV17P63A \u00b7 dsPIC33AK512MPS506 Curiosity Nano",
        "device": "dsPIC33AK512MPS506",
        "package": "VQFN/TQFP-64",
        "pin_count": 64,
        "default_core": 1,
        "default_pinsel": 0,
        # the two edge connector rows, in order from the USB end
        "edge_rows": {
            'left': [
                {
                    'dev': None,
                    'label': 'NC',
                    'port': None,
                    'pos': 1,
                },
                {
                    'dev': None,
                    'label': 'ID',
                    'port': None,
                    'pos': 2,
                },
                {
                    'dev': 45,
                    'label': 'CDC RX',
                    'port': 'RC10',
                    'pos': 3,
                },
                {
                    'dev': 46,
                    'label': 'CDC TX',
                    'port': 'RC11',
                    'pos': 4,
                },
                {
                    'dev': 41,
                    'label': 'PGC3',
                    'port': 'RC2',
                    'pos': 5,
                },
                {
                    'dev': 43,
                    'label': 'GPIO0',
                    'port': 'RC3',
                    'pos': 6,
                },
                {
                    'dev': 17,
                    'label': 'RB5',
                    'port': 'RB5',
                    'pos': 7,
                },
                {
                    'dev': 32,
                    'label': 'RB11',
                    'port': 'RB11',
                    'pos': 8,
                },
                {
                    'dev': 27,
                    'label': 'RB3',
                    'port': 'RB3',
                    'pos': 9,
                },
                {
                    'dev': 28,
                    'label': 'RB4',
                    'port': 'RB4',
                    'pos': 10,
                },
                {
                    'dev': 35,
                    'label': 'RC6',
                    'port': 'RC6',
                    'pos': 11,
                },
                {
                    'dev': 36,
                    'label': 'RC7',
                    'port': 'RC7',
                    'pos': 12,
                },
                {
                    'dev': 60,
                    'label': 'RD6',
                    'port': 'RD6',
                    'pos': 13,
                },
                {
                    'dev': 9,
                    'label': 'RA10',
                    'port': 'RA10',
                    'pos': 14,
                },
                {
                    'dev': None,
                    'label': 'GND',
                    'port': None,
                    'pos': 15,
                },
                {
                    'dev': 62,
                    'label': 'RD7',
                    'port': 'RD7',
                    'pos': 16,
                },
                {
                    'dev': 20,
                    'label': 'RB0',
                    'port': 'RB0',
                    'pos': 17,
                },
                {
                    'dev': 33,
                    'label': 'RC8',
                    'port': 'RC8',
                    'pos': 18,
                },
                {
                    'dev': 34,
                    'label': 'RC9',
                    'port': 'RC9',
                    'pos': 19,
                },
                {
                    'dev': 24,
                    'label': 'RB9',
                    'port': 'RB9',
                    'pos': 20,
                },
                {
                    'dev': 31,
                    'label': 'RB10',
                    'port': 'RB10',
                    'pos': 21,
                },
                {
                    'dev': 61,
                    'label': 'RD4',
                    'port': 'RD4',
                    'pos': 22,
                },
                {
                    'dev': 44,
                    'label': 'RC4',
                    'port': 'RC4',
                    'pos': 23,
                },
                {
                    'dev': None,
                    'label': 'GND',
                    'port': None,
                    'pos': 24,
                },
                {
                    'dev': 49,
                    'label': 'RD0',
                    'port': 'RD0',
                    'pos': 25,
                },
                {
                    'dev': 50,
                    'label': 'RD1',
                    'port': 'RD1',
                    'pos': 26,
                },
                {
                    'dev': 39,
                    'label': 'RC0',
                    'port': 'RC0',
                    'pos': 27,
                },
                {
                    'dev': 40,
                    'label': 'RC1',
                    'port': 'RC1',
                    'pos': 28,
                },
            ],
            'right': [
                {
                    'dev': None,
                    'label': 'VBUS',
                    'port': None,
                    'pos': 1,
                },
                {
                    'dev': None,
                    'label': 'VOFF',
                    'port': None,
                    'pos': 2,
                },
                {
                    'dev': None,
                    'label': 'RESET',
                    'port': None,
                    'pos': 3,
                },
                {
                    'dev': 42,
                    'label': 'PGD3',
                    'port': 'RC5',
                    'pos': 4,
                },
                {
                    'dev': None,
                    'label': 'GND',
                    'port': None,
                    'pos': 5,
                },
                {
                    'dev': None,
                    'label': 'VTG',
                    'port': None,
                    'pos': 6,
                },
                {
                    'dev': 30,
                    'label': 'RB7',
                    'port': 'RB7',
                    'pos': 7,
                },
                {
                    'dev': 12,
                    'label': 'RA2',
                    'port': 'RA2',
                    'pos': 8,
                },
                {
                    'dev': 29,
                    'label': 'RB6',
                    'port': 'RB6',
                    'pos': 9,
                },
                {
                    'dev': 7,
                    'label': 'RA8',
                    'port': 'RA8',
                    'pos': 10,
                },
                {
                    'dev': 6,
                    'label': 'RA11',
                    'port': 'RA11',
                    'pos': 11,
                },
                {
                    'dev': 3,
                    'label': 'RA1',
                    'port': 'RA1',
                    'pos': 12,
                },
                {
                    'dev': 21,
                    'label': 'RB1',
                    'port': 'RB1',
                    'pos': 13,
                },
                {
                    'dev': 1,
                    'label': 'RA0',
                    'port': 'RA0',
                    'pos': 14,
                },
                {
                    'dev': None,
                    'label': 'GND',
                    'port': None,
                    'pos': 15,
                },
                {
                    'dev': 23,
                    'label': 'RB8',
                    'port': 'RB8',
                    'pos': 16,
                },
                {
                    'dev': 22,
                    'label': 'RB2',
                    'port': 'RB2',
                    'pos': 17,
                },
                {
                    'dev': 52,
                    'label': 'RD3',
                    'port': 'RD3',
                    'pos': 18,
                },
                {
                    'dev': 51,
                    'label': 'RD2',
                    'port': 'RD2',
                    'pos': 19,
                },
                {
                    'dev': 16,
                    'label': 'RA6',
                    'port': 'RA6',
                    'pos': 20,
                },
                {
                    'dev': 15,
                    'label': 'RA5',
                    'port': 'RA5',
                    'pos': 21,
                },
                {
                    'dev': 14,
                    'label': 'RA4',
                    'port': 'RA4',
                    'pos': 22,
                },
                {
                    'dev': 13,
                    'label': 'RA3',
                    'port': 'RA3',
                    'pos': 23,
                },
                {
                    'dev': None,
                    'label': 'GND',
                    'port': None,
                    'pos': 24,
                },
                {
                    'dev': 59,
                    'label': 'RD5',
                    'port': 'RD5',
                    'pos': 25,
                },
                {
                    'dev': 8,
                    'label': 'RA9',
                    'port': 'RA9',
                    'pos': 26,
                },
                {
                    'dev': 63,
                    'label': 'RD8',
                    'port': 'RD8',
                    'pos': 27,
                },
                {
                    'dev': 2,
                    'label': 'RA7',
                    'port': 'RA7',
                    'pos': 28,
                },
            ],
        },
        "onboard": {
        49: "user LED (LED0)",
        43: "user switch (SW0)",
    },
    },
}
