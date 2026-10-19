"""Process-held receiver ownership; closing or crashing releases the OS lock."""
from contextlib import contextmanager
import os


@contextmanager
def receiver_lease(destination):
    # Keep the file: unlinking a lock lets a second process lock a different inode.
    with (destination / '.receiver.lock').open('a+b') as handle:
        handle.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError('Another receiver owns this download folder. Stop it before starting a new session.') from error
        yield
