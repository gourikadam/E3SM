import os, sys, copy
import xml.etree.ElementTree as ET

_CIMEROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..","..","..","cime")
sys.path.append(_CIMEROOT)

from CIME.utils import expect

###############################################################################
class MockCase(object):
###############################################################################
    """
    Helper function, to generate a cime case to be fed to doctest tests
    """

    def __init__(self, kv_dict):
        self._kv_dict = dict(kv_dict)

    def get_value(self, key):
        if key in self._kv_dict:
            return self._kv_dict[key]
        else:
            return None

###############################################################################
def parse_string_as_list (string):
###############################################################################
    """
    Takes a string representation of nested list and creates
    a nested list of stirng. For instance, with
        s = "(a,b,(c,d),e)
        l = parse_string_as_list
    we would have l = ['a', 'b', '(c,d)', 'e']

    >>> s = '(a,(b,c))'
    >>> l = parse_string_as_list(s)
    >>> len(l)
    2
    >>> l[0] == 'a'
    True
    >>> l[1] == '(b,c)'
    True
    >>> s = '(a,b,'
    >>> l = parse_string_as_list(s)
    Traceback (most recent call last):
    eamxx_buildnml_impl.parse_string_as_list.<locals>.UnmatchedParentheses
    >>> s = '(a,(b)'
    >>> l = parse_string_as_list(s)
    Traceback (most recent call last):
    eamxx_buildnml_impl.parse_string_as_list.<locals>.UnmatchedParentheses
    """

    class UnmatchedParentheses (Exception):
        pass

    if string[0]!='(' or string[-1]!=')':
        raise UnmatchedParentheses

    sub_open = string.find('(',1)
    sub_close = string.rfind(')',0,-1)
    if not (sub_open>=0)==(sub_close>=0):
        raise UnmatchedParentheses

    # Prevent empty string to pollute s.split()
    my_split = lambda str : [s for s in str.split(',') if s.strip() != '']

    if sub_open>=0:
        l = []
        l.extend(my_split(string[1:sub_open-1]))
        l.append(string[sub_open:sub_close+1])
        l.extend(my_split(string[sub_close+2:-1]))
    else:
        l = my_split(string[1:-1])

    return l

###############################################################################
def find_node (root,name):
###############################################################################
    """
    Finds node with given name inside the root element, with a depth-search
    strategy (i.e., follow children before siblings).
    WARNING: this function does not check for uniqueness. If there are
             multiple matches, the first match is returned.
    >>> xml = '''
    ... <my_root>
    ...   <a>1</a>
    ...   <b>
    ...     <c>2</c>
    ...   </b>
    ... </my_root>
    ... '''
    >>> import xml.etree.ElementTree as ET
    >>> root = ET.fromstring(xml)
    >>> find_node(root,'d')==None
    True
    >>> find_node(root,'c').text
    '2'
    """

    if root.tag==name:
        return root

    for elem in root:
        found = find_node(elem,name)
        if found is not None:
            return found

    return None

###############################################################################
def get_child (root,name,remove=False):
###############################################################################
    """
    Get children with given name. If not found, throws an exception.
    Optionally, the child can be removed from the parent.
    >>> xml = '''
    ... <my_root>
    ...   <a>1</a>
    ...   <b>
    ...     <c>2</c>
    ...   </b>
    ... </my_root>
    ... '''
    >>> import xml.etree.ElementTree as ET
    >>> root = ET.fromstring(xml)
    >>> get_child(root,'c')
    Traceback (most recent call last):
    eamxx_buildnml_impl.get_child.<locals>.XmlNodeNotFound: ERROR: There must be exactly one c entry inside my_root
    """

    class XmlNodeNotFound (Exception):
        pass

    expect (len(root.findall(name))==1,
            "There must be exactly one {} entry inside {}".format(name,root.tag),
            XmlNodeNotFound)
    child = root.find(name)
    if remove:
        root.remove(child)

    return child

###############################################################################
def has_child (root,name):
###############################################################################
    """
    Check if root element has a *direct* child with given name
    >>> xml = '''
    ... <my_root>
    ...   <a>1</a>
    ...   <b>
    ...     <c>2</c>
    ...   </b>
    ... </my_root>
    ... '''
    >>> import xml.etree.ElementTree as ET
    >>> root = ET.fromstring(xml)
    >>> has_child(root,'c')
    False
    >>> has_child(root,'b')
    True
    """

    return False if root.find(name) is None else True

###############################################################################
def refine_type(entry, force_type=None):
###############################################################################
    """
    Try to convert the text entry to the appropriate type based on its contents.
    >>> e = '(a,b)'
    >>> refine_type(e)==e
    True
    >>> e = '[a,b]'
    >>> refine_type(e)==e
    True
    >>> e = 'a,b'
    >>> refine_type(e)==['a','b']
    True
    >>> e = 'true,falsE'
    >>> refine_type(e)==[True,False]
    True
    >>> e = '1'
    >>> refine_type(e,force_type='real')==1.0
    True
    >>> e = '1,b'
    >>> refine_type(e)==[1,'b',True]
    Traceback (most recent call last):
    eamxx_buildnml_impl.refine_type.<locals>.InconsistentListTypes: ERROR: List '1,b' has inconsistent types inside
    >>> e = '1.0'
    >>> refine_type(e,force_type='my_type')
    Traceback (most recent call last):
    eamxx_buildnml_impl.refine_type.<locals>.BadForceType: Bad force_type: my_type
    """
    # We want to preserve strings representing lists
    if (entry[0]=="(" and entry[-1]==")") or \
       (entry[0]=="[" and entry[-1]=="]") :
        return entry

    class InconsistentListTypes (Exception):
        pass
    class BadForceType (Exception):
        pass
    class ForceTypeUnmet (Exception):
        pass

    if "," in entry:
        result = [refine_type(item.strip(), force_type=force_type) for item in entry.split(",") if item.strip() != ""]
        expected_type = type(result[0])
        for item in result[1:]:
            expect(isinstance(item, expected_type),
                  "List '{}' has inconsistent types inside".format(entry),
                  InconsistentListTypes)

        return result

    if force_type:
        try:
            if force_type == "logical":
                if entry.upper() == "TRUE":
                    return True
                elif entry.upper() == "FALSE":
                    return False
                else:
                    return bool(int(entry))

            elif force_type == "integer":
                return int(entry)
            elif force_type == "real":
                return float(entry)
            elif force_type == "string":
                return str(entry)
            else:
                raise BadForceType ("Bad force_type: {}".format(force_type))
                return None

        except ValueError:
            raise ForceTypeUnmet ("Could not use '{}' as type '{}'".format(entry, force_type))
            return None

    if entry.upper() == "TRUE":
        return True
    elif entry.upper() == "FALSE":
        return False

    try:
        v = int(entry)
        return v
    except ValueError:
        pass

    try:
        v = float(entry)
        return v
    except ValueError:
        return entry

###############################################################################
def derive_type(entry):
###############################################################################
    """
    Try to determine the type that the input string is representing
    >>> derive_type('1')
    'integer'
    >>> derive_type('1.0')
    'real'
    >>> derive_type('one')
    'string'
    """
    class UnrecognizedType (Exception):
        pass

    refined_value = refine_type(entry)
    if isinstance(refined_value, list):
        refined_value = refined_value[0]

    if isinstance(refined_value, bool):
        return "logical"
    elif isinstance(refined_value, int):
        return "integer"
    elif isinstance(refined_value, float):
        return "real"
    elif isinstance(refined_value, str):
        return "string"
    else:
        raise(UnrecognizedType, "Couldn't derive type of '{}'".format(entry))
        return None

###############################################################################
def check_value(elem, value):
###############################################################################
    """
    Check that a parameter's value is in the valid list

    >>> xml = '''
    ... <a type="integer" valid_values="1,2">1</a>
    ... '''
    >>> import xml.etree.ElementTree as ET
    >>> root = ET.fromstring(xml)
    >>> check_value(root,'1.0')
    Traceback (most recent call last):
    eamxx_buildnml_impl.refine_type.<locals>.ForceTypeUnmet: Could not use '1.0' as type 'integer'
    >>> check_value(root,'3')
    Traceback (most recent call last):
    eamxx_buildnml_impl.check_value.<locals>.ValidValueConstraintViolated: ERROR: Invalid value '3' for element 'a'. Value not in the valid list ('[1, 2]')

    """
    class TypeConstraintViolated (Exception):
        pass
    class ValidValueConstraintViolated (Exception):
        pass

    v = value
    vtype = None
    if "type" in elem.attrib.keys():
        vtype = elem.attrib["type"]
        v = refine_type(v,force_type=vtype)

        expect (v is not None,
                "Error! Value '{}' for element '{}' does not satisfy the constraint type={}"
                .format(value,elem.tag,vtype) +
                "  NOTE: this error should have been caught earlier! Please, contact developers.",
                TypeConstraintViolated)

    if "valid_values" in elem.attrib.keys():
        valids_str = elem.attrib["valid_values"]
        valids = [refine_type(item.strip(), force_type=vtype) for item in valids_str.split(",")]
        expect(v in valids,
                "Invalid value '{}' for element '{}'. Value not in the valid list ('{}')".format(value, elem.tag, valids),
                ValidValueConstraintViolated)

###############################################################################
def check_all_values(root):
###############################################################################
    """
    Check that all values in the xml tree do not violate their metadata
    """

    has_children = len(root)>0
    if has_children:
        for c in root:
            check_all_values(c)
    else:
        if "type" not in root.attrib.keys():
            root.attrib["type"] = derive_type(root.text)
        check_value(root,root.text)

###############################################################################
def resolve_inheritance (root,elem):
###############################################################################
    """
    If elem inherits from another node within $root, this function adds all
    children of its "parent" to elem. If parent also inherits, first
    resolve parent recursively. If parent is not found, throw an exception
    >>> xml = '''
    ... <my_root>
    ...   <base>
    ...     <a>2</a>
    ...   </base>
    ...   <derived inherit="base">
    ...   </derived>
    ... </my_root>
    ... '''
    >>> import xml.etree.ElementTree as ET
    >>> root = ET.fromstring(xml)
    >>> d = get_child(root,'derived')
    >>> len(d)
    0
    >>> resolve_inheritance(root,d)
    >>> len(d)
    1
    >>> get_child(d,'a').text
    '2'
    """

    if "inherit" in elem.attrib.keys():
        parent_name = elem.attrib["inherit"]

        parent = find_node(root,parent_name)
        expect (elem is not None,
                "Error! Parent {} of {} not found within root {}"
                .format(parent_name,elem.tag,root.tag))

        # Make sure the parent is fully resolved
        resolve_inheritance(root,parent)

        del elem.attrib["inherit"]

        for entry in parent:
            # Add the parent's default only if this element does not
            # have a more specialized version
            if not has_child(elem,entry.tag):
                new_entry = copy.deepcopy(entry)
                elem.append(new_entry)

    for child in elem:
        resolve_inheritance(root,child)

###############################################################################
def resolve_all_inheritances (root):
###############################################################################
    """
    Resolve all inheritances in the root tree
    """

    for elem in root:
        resolve_inheritance(root,elem)

###############################################################################
def get_valid_selectors(xml_root):
###############################################################################
    """
    Extract the <selector> node from the xml root, verifying
    its integrity, and returning selectors as a dict.

    >>> xml = '''
    ... <namelist_defaults>
    ...   <selectors>
    ...     <selector name="S1" case_env="ENV1"/>
    ...     <selector name="S2" case_env="ENV2"/>
    ...   </selectors>
    ... </namelist_defaults>
    ... '''
    >>> import xml.etree.ElementTree as ET
    >>> root = ET.fromstring(xml)
    >>> selectors = get_valid_selectors(root)
    >>> len(selectors)
    2
    >>> xml = '''
    ... <namelist_defaults>
    ...   <selectors>
    ...     <blah name="S1" case_env="ENV1"/>
    ...   </selectors>
    ... </namelist_defaults>
    ... '''
    >>> import xml.etree.ElementTree as ET
    >>> root = ET.fromstring(xml)
    >>> selectors = get_valid_selectors(root)
    Traceback (most recent call last):
    eamxx_buildnml_impl.get_valid_selectors.<locals>.BadSelectorTag: ERROR: Expected selector tag, not blah
    """

    class BadSelectorTag (Exception):
        pass

    # Get the right XML element, and iterate over its children
    selectors_elem = get_child(xml_root,"selectors",remove=True)
    selectors = {}
    for selector in selectors_elem:
        expect(selector.tag == "selector",
               "Expected selector tag, not {}".format(selector.tag),
               BadSelectorTag)

        selector_name  = selector.attrib["name"]
        selector_env   = selector.attrib["case_env"]
        if "regex" in selector.attrib:
            selector_regex = selector.attrib["regex"]
        else:
            selector_regex = "(.*)" # Just grab the whole thing

        selectors[selector_name] = (selector_env, selector_regex)

    return selectors 

###############################################################################
def gen_group_processes (ap_names_str, atm_procs_defaults):
###############################################################################
    """
    Given a (possibly nested) string representation of an atm group,
    generates the corresponding atm processes as XML nodes.
    """

    group = ET.Element("__APG__")

    ap_names_list = parse_string_as_list(ap_names_str)
    for ap in ap_names_list:
        # The current ap can be itself a group if either:
        #  - ap = "(ap1,ap2,...,apXYZ)", with each ap possibly itself a group string.
        #    This group is built on the fly based on the building blocks specs.
        #  - ap is declared in the XML defaults as an atm proc group (which must store
        #    the 'atm_procs_list' child, with the string representation of the group.

        if ap[0]=='(':
            # Create the atm proc group
            proc = gen_atm_proc_group(ap,atm_procs_defaults)
        else:
            # Get defaults
            proc = copy.deepcopy(get_child(atm_procs_defaults,ap))

            # Check if this pre-defined proc is itself a group, and, if so,
            # build all its sub-processes
            if has_child(proc,"Type") and get_child(proc,"Type").text=="Group":
                # This entry of the group is itself a group, with pre-defined
                # defaults. Let's add its entries to it
                sub_group_procs = get_child(proc,"atm_procs_list").text
                proc.extend(gen_group_processes(sub_group_procs,atm_procs_defaults))

        # Append subproc to group
        group.append(proc)

    return group

###############################################################################
def gen_atm_proc_group(atm_procs_list, atm_procs_defaults):
###############################################################################
    """
    Given a (possibly nested) list of atm procs names, and the defaults
    section for each atm proc, builds an XML node containing the tree
    representing the atm process group, with nodes including APG parameters
    as well as one sub-node for each atm proc in the group
    >>> xml = '''
    ... <ap>
    ...   <atm_proc_group>
    ...     <prop1>1</prop1>
    ...     <atm_procs_list>THE_LIST</atm_procs_list>
    ...   </atm_proc_group>
    ...   <ap1>
    ...   </ap1>
    ...   <ap2>
    ...     <prop1>2</prop1>
    ...     <prop2>3</prop2>
    ...   </ap2>
    ...   <my_group inherit="atm_proc_group">
    ...     <atm_procs_list>(p1,ap2)</atm_procs_list>
    ...   </my_group>
    ... </ap>
    ... '''
    >>> import xml.etree.ElementTree as ET
    >>> defaults = ET.fromstring(xml)
    >>> ap_list = '(ap1,(ap2,ap1))'
    >>> apg = gen_atm_proc_group(ap_list,defaults)
    >>> get_child(apg,'atm_procs_list').text==ap_list
    True
    >>>
    >>> has_child(apg,'group.ap2_ap1.')
    True
    >>> has_child(apg,'prop1') and get_child(apg,'prop1').text=="1"
    True
    """

    # Set defaults from atm_proc_group
    group = ET.Element("__APG__")
    group.attrib["inherit"] = "atm_proc_group"
    resolve_inheritance(atm_procs_defaults,group)
    get_child(group,"atm_procs_list").text = atm_procs_list

    # Create processes
    group_procs = gen_group_processes (atm_procs_list, atm_procs_defaults)

    # Append procs and generate name for the group.
    # NOTE: the name of a 'generic' group is 'group.AP1_AP2_..._APN.'
    names = []
    for c in group_procs:
        names.append(c.tag)
        group.append(c)
    group.tag = "group." + '_'.join(names) + '.'
    
    return group
