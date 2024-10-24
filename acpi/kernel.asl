DefinitionBlock ("kernel.aml", "SSDT", 2, "uACPI", "uACPI-OS", 0xCAFEBABE)
{
    #include "list.asl"

    //
    // Kernel ENTry
    //
    Method (KENT) {

        Local0 = Package(1){}
        Local1 = Package(3){ 0, 1, 2 }
        Local2 = Package(2){ 0, 1 }

        Local0[0] = RefOf(Local1)
        CopyObject(RefOf(Local2), Local0[0])

        Debug = Local0

        // Method (TEST, 1) {
        //     Debug = Arg0
        // }

        // Debug = DerefOf(Local0[0])
        // TEST(DerefOf(Local0[0]))

        // // create a list
        // Local0 = LNEW()

        // // add an element to it 
        // Local1 = NEW_LIST_ENTRY
        // LADD(Local0, Local1)
        // Local1[ LIST_ENTRY_VALUE ] = 0xBABE

        // Debug = Local0
        // Debug = Local1

        // // now iterate it 
        // Local1 = DerefOf(Local0[ LIST_ENTRY_NEXT ])
        // While (DerefOf(Local1[ LIST_ENTRY_ID ]) != DerefOf(Local0[ LIST_ENTRY_ID ])) 
        // {
        //     // print the element
        //     Debug = Local1
        //     Debug = DerefOf(Local1[ LIST_ENTRY_VALUE ])

        //     // go to the next element 
        //     Local1 = DerefOf(Local1[ LIST_ENTRY_NEXT ])
        // }
    }
}