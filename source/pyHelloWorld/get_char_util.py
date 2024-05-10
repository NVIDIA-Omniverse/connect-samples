# SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: MIT
#


class CharInput:
    """Helper class for obtaining a keyboard input character in a cross-platform manner."""

    _getCharFunc = None

    @staticmethod
    def getCharWindows():
        """Get the user key input on Windows.

        Returns:
            (str) Input char.
        """
        import msvcrt

        # Reads a single key press from the keyboard and returns the string of that key
        return msvcrt.getch()

    @staticmethod
    def getCharUnixLike():
        """Get the user key input on Unix-like platforms.

        Returns:
            (str) Input char.
        """
        import sys
        import termios
        import tty

        # File descriptor for stdin
        fd = sys.stdin.fileno()
        # Get the current terminal settings
        old_settings = termios.tcgetattr(fd)
        try:
            # Set the terminal to "cbreak" mode. Allowing Ctrl+C for interrupt.
            tty.setcbreak(fd)
            # Read 1 byte (a single character) of input
            ch = sys.stdin.read(1)
        finally:
            # Restore the original terminal settings after pending output operations are completed.
            termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
        # Return the character read (if any)
        return ch.encode("ascii")

    @classmethod
    def getChar(cls):
        """Get the user key input.

        Returns:
            (str) Input char.
        """
        if cls._getCharFunc:
            return cls._getCharFunc()

        try:
            # Try Windows first
            cls._getCharFunc = cls.getCharWindows
            return cls._getCharFunc()

        except ImportError:
            cls._getCharFunc = cls.getCharUnixLike
            return cls._getCharFunc()


def getChar():
    """Get the user key input.

    Returns:
        (str) Input char.
    """
    return CharInput.getChar()
