//////////////////////////////////////////////////////////////////////////////
// Linked list implementation
//////////////////////////////////////////////////////////////////////////////

#define NEW_LIST_ENTRY Package(4){}
#define LIST_ENTRY_NEXT 0
#define LIST_ENTRY_PREV 1
#define LIST_ENTRY_ID 2
#define LIST_ENTRY_VALUE 3

//
// List ID Generator
//
Name(LIDG, 1)

//
// List NEW
//
// Arg0 - the head
//
Method (LNEW, 0, Serialized)
{
    Local0 = Package(3){}
    Local0[ LIST_ENTRY_NEXT ] = RefOf(Local0)
    Local0[ LIST_ENTRY_PREV ] = RefOf(Local0)
    Local0[ LIST_ENTRY_ID ] = LIDG
    LIDG++
    Return (Local0)
}

//
// List ADd Common
//
// Arg0 - new
// Arg1 - prev
// Arg2 - next
//
Method (LADC, 3) 
{
    Debug = Arg0
    Debug = Arg1
    Debug = Arg2
    Arg2[ LIST_ENTRY_PREV ] = RefOf(Arg0)
    Arg0[ LIST_ENTRY_NEXT ] = RefOf(Arg2)
    Arg0[ LIST_ENTRY_PREV ] = RefOf(Arg1)
    Arg1[ LIST_ENTRY_NEXT ] = RefOf(Arg0)
}

//
// List ADD
//
// Arg0 - head
// Arg1 - entry
//
Method (LADD, 2) 
{
    Arg1[ LIST_ENTRY_ID ] = 0
    LADC(Arg1, Arg0, DerefOf(Arg0[ LIST_ENTRY_NEXT ]))
}
