"""hifibench: bring-up and test of the hifi receiver boards from a laptop.

docs/bench.md says what each piece does and how the bench is wired.
"""
API = "/api/v1"
SERVICE = "_hifi._tcp.local."
CH340 = (0x1A86, 0x7523)  # the CH340C on every board's USB-C
