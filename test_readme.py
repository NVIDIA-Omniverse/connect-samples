import re
import unittest
from pathlib import Path

README = Path(__file__).with_name('README.md')


def code_blocks(language):
    text = README.read_text(encoding='utf-8')
    pattern = re.compile(rf'^```{language}\n(.*?)\n```$', re.MULTILINE | re.DOTALL)
    return pattern.findall(text)


def extract_call(block, name):
    match = re.search(rf'{re.escape(name)}\s*\(', block)
    if not match:
        raise AssertionError(f'No call to {name} found')
    start = match.end()
    depth = 1
    i = start
    while i < len(block) and depth > 0:
        if block[i] == '(':
            depth += 1
        elif block[i] == ')':
            depth -= 1
        i += 1
    if depth != 0:
        raise AssertionError(f'Unbalanced parentheses in {name} call')
    return block[start:i - 1]


class TestReadmeSnippets(unittest.TestCase):
    def test_cpp_omni_client_list_has_callback_and_userdata(self):
        for block in code_blocks('cpp'):
            if 'omniClientList(' not in block:
                continue
            args = extract_call(block, 'omniClientList')
            self.assertNotIn(
                '&retCode',
                args,
                'omniClientList does not accept an int* retCode argument',
            )
            self.assertIn(
                'nullptr',
                args,
                'omniClientList should pass the callback and a void* userData',
            )


if __name__ == '__main__':
    if not README.exists():
        raise FileNotFoundError(f'README.md not found next to {__file__}')
    unittest.main()
