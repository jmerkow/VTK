"""Test string and unicode support in VTK-Python

The following string features have to be tested for string and unicode
- Pass a string arg by value
- Pass a string arg by reference
- Return a string arg by value
- Return a string arg by reference

The following features are not supported
- Pointers to strings, arrays of strings
- Passing a string arg by reference and returning a value in it

Created on May 12, 2010 by David Gobbi
"""

import sys
import vtk
from vtk.test import Testing

if sys.hexversion > 0x03000000:
    cedilla = 'Fran\xe7ois'
    nocedilla = 'Francois'
else:
    cedilla = unicode('Fran\xe7ois', 'latin1')
    nocedilla = unicode('Francois')


class TestString(Testing.vtkTest):
    def testPassByValue(self):
        """Pass string by value... hard to find examples of this,
        because "const char *" methods shadow "vtkStdString" methods.
        """
        self.assertEqual('y', 'y')

    def testReturnByValue(self):
        """Return a string by value."""
        a = vtk.vtkArray.CreateArray(1, vtk.VTK_INT)
        a.Resize(1,1)
        a.SetDimensionLabel(0, 'x')
        s = a.GetDimensionLabel(0)
        self.assertEqual(s, 'x')

    def testPassByReference(self):
        """Pass a string by reference."""
        a = vtk.vtkArray.CreateArray(0, vtk.VTK_STRING)
        a.SetName("myarray")
        s = a.GetName()
        self.assertEqual(s, "myarray")

    def testReturnByReference(self):
        """Return a string by reference."""
        a = vtk.vtkStringArray()
        s = "hello"
        a.InsertNextValue(s)
        t = a.GetValue(0)
        self.assertEqual(t, s)

    def testPassAndReturnUnicodeByReference(self):
        """Pass a unicode string by const reference"""
        a = vtk.vtkUnicodeStringArray()
        a.InsertNextValue(cedilla)
        u = a.GetValue(0)
        self.assertEqual(u, cedilla)

    def testPassStringAsUnicode(self):
        """Pass a string when unicode is expected.  Should fail."""
        a = vtk.vtkUnicodeStringArray()
        self.assertRaises(TypeError,
                          a.InsertNextValue, ('Francois',))

    def testPassUnicodeAsString(self):
        """Pass a unicode where a string is expected.  Should succeed."""
        a = vtk.vtkStringArray()
        a.InsertNextValue(nocedilla)
        s = a.GetValue(0)
        self.assertEqual(s, 'Francois')

if __name__ == "__main__":
    Testing.main([(TestString, 'test')])
