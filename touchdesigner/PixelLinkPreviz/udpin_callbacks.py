# udpin_callbacks — paste into the Callbacks DAT of the UDP In DAT
#
# UDP In DAT settings: Port 5150, Row/Callback Format = One Per Message,
# Maximum Lines = 1 (the table itself isn't used).
#
# The arguments are taken loosely so this works whichever way your
# TouchDesigner version passes them: the raw payload is the bytes argument,
# and the peer is used to reply (INFO and STATUS) to the master.

core = op('pl_core').module


def onReceive(dat, rowIndex, message, *args, **kwargs):
    data = None
    peer = None
    for a in list(args) + list(kwargs.values()):
        if isinstance(a, (bytes, bytearray)) and data is None:
            data = bytes(a)
        elif (isinstance(a, (list, tuple)) and data is None and a
              and all(isinstance(v, int) for v in a)):
            data = bytes(a)
        elif hasattr(a, 'sendBytes') and peer is None:
            peer = a
    if data is None:
        # Fallback: some setups only hand over the message text.
        data = message.encode('latin-1') if isinstance(message, str) else bytes(message)
    core.handle(data, peer.sendBytes if peer else None)
    return
