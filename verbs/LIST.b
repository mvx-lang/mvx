* MVX — a native compiler and runtime for Pick/MultiValue BASIC.
* Copyright (C) 2026 Gordon Heydon.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as
* published by the Free Software Foundation.  There is NO WARRANTY, to
* the extent permitted by law; see the LICENSE file for details.
*
* SPDX-License-Identifier: GPL-2.0-only
* LIST file {items...} {WITH item op value} {BY item}
* Columns come from dictionary D-items:
*   1 = D, 2 = attribute number, 3 = conversion (OCONV code),
*   4 = column heading, 5 = format e.g. "12L" / "8R".
S = TRIM(SENTENCE())
* DESCRIBE / EXPLAIN may sit right after the verb (LIST DESCRIBE file …) or
* trail the sentence; either way it prints the query plan instead of running.
DESC = 0
DW = FIELD(S, " ", 2)
IF DW = "DESCRIBE" OR DW = "EXPLAIN" THEN
   DESC = 1
   S = FIELD(S, " ", 1):" ":FIELD(S, " ", 3, 9999)
END
DICTF = 0
FN = FIELD(S, " ", 2)
TBASE = 3
IF FN = "DICT" THEN
   DICTF = 1
   FN = FIELD(S, " ", 3)
   TBASE = 4
END
IF FN = "" THEN
   PRINT "usage: LIST {DICT} file {items} {WITH item op value} {BY item}"
   STOP
END
IF DICTF THEN
   OPEN "DICT", FN TO F ELSE
      PRINT "cannot open DICT ":FN
      STOP
   END
END ELSE
   OPEN FN TO F ELSE
      PRINT "cannot open ":FN
      STOP
   END
END
* column definitions come from the file's dictionary; a dictionary
* listing has no dict-of-dict, so only @ID resolves there
DOPEN = 0
IF DICTF = 0 THEN
   DOPEN = 1
   OPEN "DICT", FN TO DC ELSE DOPEN = 0
END

* ---- parse the sentence ------------------------------------------------
* WITH conditions accumulate (WITH a op b {AND c op d ...}); they are ANDed.
NT = DCOUNT(S, " ")
COLS = ""
NW = 0
WIS = ""
WOPS = ""
WVS = ""
BYI = ""
I = TBASE
LOOP
WHILE I <= NT DO
   T = FIELD(S, " ", I)
   BEGIN CASE
   CASE T = "WITH" OR T = "AND"
      NW = NW + 1
      WIS<NW> = FIELD(S, " ", I + 1)
      WOPS<NW> = FIELD(S, " ", I + 2)
      WVS<NW> = FIELD(S, " ", I + 3)
      I = I + 3
   CASE T = "DESCRIBE" OR T = "EXPLAIN"
      DESC = 1
   CASE T = "BY"
      BYI = FIELD(S, " ", I + 1)
      I = I + 1
   CASE 1
      COLS<-1> = T
   END CASE
   I = I + 1
REPEAT
FOR K = 1 TO NW
   V = WVS<K>
   IF LEN(V) >= 2 THEN
      Q = V[1, 1]
      IF Q = "'" OR Q = '"' THEN WVS<K> = V[2, LEN(V) - 2]
   END
NEXT K

* ---- expand phrases -----------------------------------------------------
* A PH item is not a column.  Its attribute 2 is a LIST OF COLUMN NAMES, not
* an attribute number, so resolving one as a D item reads the member list as
* a number and the runtime stops with "non-numeric value ... used in numeric
* context".  Expand it into the columns it stands for instead.
*
* With nothing named at all, the file's own default list applies: %PH% in the
* dictionary, beside %FILE% and %MAP%, and hidden from a DICT listing by the
* same leading-% rule.  That is the open-account spelling of the default
* column list other MV systems keep as `@` (mvx#164).
*
* One level, deliberately: a phrase naming another phrase is a loop waiting
* to happen, and no MV system this has to agree with resolves them either.
* Name it rather than splitting it here: %PH% is itself a PH item, so the
* expansion below turns it into its members and there is one splitter.
IF COLS = "" AND DOPEN THEN
   READ PHD FROM DC, "%PH%" THEN
      IF PHD<1>[1, 1] = "P" THEN COLS = "%PH%"
   END
END
IF DOPEN THEN
   NEWC = ""
   PN = DCOUNT(COLS, @AM)
   FOR P = 1 TO PN
      PNM = COLS<P>
      PEX = 0
      IF PNM # "@ID" THEN
         READ PD FROM DC, PNM THEN
            IF PD<1>[1, 1] = "P" THEN
               PML = TRIM(PD<2>)
               PMN = DCOUNT(PML, " ")
               FOR PM = 1 TO PMN
                  PMM = FIELD(PML, " ", PM)
                  IF PMM # "" THEN NEWC<-1> = PMM
               NEXT PM
               PEX = 1
            END
         END
      END
      IF PEX = 0 THEN NEWC<-1> = PNM
   NEXT P
   COLS = NEWC
END

* ---- resolve dictionary items ------------------------------------------
* Column layout: parallel dynamic arrays; slot 0 conventions: attr no 0
* means the record id itself.
CN = DCOUNT(COLS, @AM)
ANOS = ""
ISPECS = ""
CONVS = ""
HEADS = ""
MASKS = ""
ASSOCS = ""
BAD = ""
FOR C = 1 TO CN
   NM = COLS<C>
   ANO = ""
   ISP = ""
   CV = ""
   HD = NM
   MK = "L#10"
   ASN = ""
   IF NM = "@ID" THEN
      ANO = 0
   END ELSE
      GOT = 0
      IF DOPEN THEN
         READ DI FROM DC, NM THEN
            IF DI<1>[1, 1] = "I" THEN
               ANO = -1
               ISP = DI<2>
            END ELSE
               ANO = DI<2>
            END
            CV = DI<3>
            IF DI<4> # "" THEN
               HD = DI<4>
            END
            FM = DI<5>
            IF FM # "" THEN
               J = FM[LEN(FM), 1]
               W = FM[1, LEN(FM) - 1]
               IF J = "R" THEN
                  MK = "R#":W
               END ELSE
                  MK = "L#":W
               END
            END
            ASN = DI<6>
            GOT = 1
         END
      END
      IF GOT = 0 THEN
         BAD = NM
      END
   END
   ANOS<C> = ANO
   ISPECS<C> = ISP
   CONVS<C> = CV
   HEADS<C> = HD
   MASKS<C> = MK
   ASSOCS<C> = ASN
NEXT C
IF BAD # "" THEN
   PRINT BAD:" is not a dictionary item in ":FN
   STOP
END

* ---- a dictionary listing describes its items ----------------------------
* A DICT listing has no dictionary of its own to resolve columns from, so
* with nothing named it showed @ID and nothing else -- the one listing where
* what you want IS the record.  Describe the D-item shape instead, which is
* what UniData gives you by shipping the phrase in its dict-of-dicts.
*
* Set here rather than through COLS because there is nothing to look these
* up in: the attribute numbers ARE the definition.  S/M before the
* association, so the pair reads the way it does on the other systems.
IF DICTF AND CN = 0 THEN
   CN = 7
   ANOS   = 1:@AM:2:@AM:3:@AM:4:@AM:5:@AM:7:@AM:6
   ISPECS = "":@AM:"":@AM:"":@AM:"":@AM:"":@AM:"":@AM:""
   CONVS  = "":@AM:"":@AM:"":@AM:"":@AM:"":@AM:"":@AM:""
   ASSOCS = "":@AM:"":@AM:"":@AM:"":@AM:"":@AM:"":@AM:""
   HEADS  = "Type":@AM:"Attr":@AM:"Conv":@AM:"Heading":@AM:"Format":@AM:"S/M":@AM:"Assoc"
   * Attr is LEFT-justified though it usually holds a number: attribute 2 of
   * an I item is its expression and of a PH item its member list, and
   * right-justifying those shows the last few characters -- "RICE" out of
   * EPRICE -- where the first few at least say what it is.
   MASKS  = "L#4":@AM:"L#8":@AM:"L#5":@AM:"L#16":@AM:"L#6":@AM:"L#3":@AM:"L#12"
END


* WITH item resolution — one attribute (or I-descriptor) per condition
WANOS = ""
WSPECS = ""
FOR K = 1 TO NW
   WI = WIS<K>
   WANO = ""
   WSPEC = ""
   IF WI = "@ID" THEN
      WANO = 0
   END ELSE
      GOT = 0
      IF DOPEN THEN
         READ DI FROM DC, WI THEN
            IF DI<1>[1, 1] = "I" THEN
               WANO = -1
               WSPEC = DI<2>
            END ELSE
               WANO = DI<2>
            END
            GOT = 1
         END
      END
      IF GOT = 0 THEN
         PRINT WI:" is not a dictionary item in ":FN
         STOP
      END
   END
   WANOS<K> = WANO
   WSPECS<K> = WSPEC
NEXT K

* BY item resolution; the dict item's justification decides the sort
* order — R-formatted fields sort right-justified (numeric).
BANO = ""
BSPEC = ""
BORD = "AL"
IF BYI # "" THEN
   IF BYI = "@ID" THEN
      BANO = 0
   END ELSE
      GOT = 0
      IF DOPEN THEN
         READ DI FROM DC, BYI THEN
            IF DI<1>[1, 1] = "I" THEN
               BANO = -1
               BSPEC = DI<2>
            END ELSE
               BANO = DI<2>
            END
            FM = DI<5>
            IF FM # "" THEN
               IF FM[LEN(FM), 1] = "R" THEN
                  BORD = "AR"
               END
            END
            GOT = 1
         END
      END
      IF GOT = 0 THEN
         PRINT BYI:" is not a dictionary item in ":FN
         STOP
      END
   END
END

* ---- DESCRIBE: show the query plan, don't run it -----------------------
* Render how the backend would run this WITH/BY query and stop; the plan
* mirrors the push-down the verb would choose.
IF DESC THEN
   PSPEC = ""
   FOR K = 1 TO NW
      PSPEC<K> = WANOS<K>:@VM:WOPS<K>:@VM:WVS<K>
   NEXT K
   BB = 0
   IF BYI # "" AND BANO > 0 THEN BB = BANO
   BNUM = 0
   IF BORD = "AR" THEN BNUM = 1
   OSPEC = BB:@VM:BNUM:@VM:0
   PLAN = DESCRIBE(F, PSPEC, OSPEC)
   PRINT PLAN
   STOP
END

* ---- scan, filter, order -----------------------------------------------
* use the active select list when one exists, classic style
IF SYSTEM(11) = 0 THEN
   IXUSED = 0
   * multi-condition WITH -> one WHERE (all-or-nothing); on 0 the verb filters
   IF NW >= 1 THEN
      PSPEC = ""
      FOR K = 1 TO NW
         PSPEC<K> = WANOS<K>:@VM:WOPS<K>:@VM:WVS<K>
      NEXT K
      IXUSED = MULTISELECT(F, PSPEC)
   END
   IF IXUSED = 0 AND NW = 1 THEN
      * single-condition fallbacks: TRANS() JOIN, then the LMDB equality index
      IF WANOS<1> = -1 AND WSPECS<1>[1, 6] = "TRANS(" THEN
         IXUSED = TRANSSELECT(F, WSPECS<1>, WOPS<1>, WVS<1>)
      END
      IF IXUSED = 0 AND WOPS<1> = "=" AND WANOS<1> > 0 THEN
         IXUSED = INDEXSELECT(F, WIS<1>, WVS<1>)
      END
   END
   IF IXUSED THEN
      NW = 0
   END ELSE
      SELECT F
   END
END
IDS = ""
KEYS = ""
DONE = 0
LOOP
   READNEXT ID ELSE DONE = 1
UNTIL DONE DO
   READ R FROM F, ID ELSE R = ""
   OK = 1
   * control records (%FILE%, %INDEXES%, ...) are metadata, not fields
   IF DICTF AND ID[1, 1] = "%" THEN OK = 0
   FOR K = 1 TO NW
      IF OK THEN
         BEGIN CASE
         CASE WANOS<K> = 0
            RV = ID
         CASE WANOS<K> = -1
            RV = IEVAL(R, WSPECS<K>)
         CASE 1
            RV = R<WANOS<K>>
         END CASE
         WOP = WOPS<K>
         WV = WVS<K>
         CK = 0
         BEGIN CASE
         CASE WOP = "="
            IF RV = WV THEN CK = 1
         CASE WOP = "#"
            IF RV # WV THEN CK = 1
         CASE WOP = ">"
            IF RV > WV THEN CK = 1
         CASE WOP = "<"
            IF RV < WV THEN CK = 1
         CASE WOP = ">="
            IF RV >= WV THEN CK = 1
         CASE WOP = "<="
            IF RV <= WV THEN CK = 1
         END CASE
         IF CK = 0 THEN OK = 0
      END
   NEXT K
   IF OK THEN
      IF BYI # "" THEN
         BEGIN CASE
         CASE BANO = 0
            K = ID
         CASE BANO = -1
            K = IEVAL(R, BSPEC)
         CASE 1
            K = R<BANO>
         END CASE
         LOCATE(K, KEYS; POS; BORD) THEN
            KEYS = INSERT(KEYS, POS, 0, 0, K)
            IDS = INSERT(IDS, POS, 0, 0, ID)
         END ELSE
            KEYS = INSERT(KEYS, POS, 0, 0, K)
            IDS = INSERT(IDS, POS, 0, 0, ID)
         END
      END ELSE
         IDS<-1> = ID
      END
   END
REPEAT

* ---- report ------------------------------------------------------------
HDR = FMT("@ID", "L#12")
FOR C = 1 TO CN
   HDR = HDR:" ":FMT(HEADS<C>, MASKS<C>)
NEXT C
PRINT HDR
N = DCOUNT(IDS, @AM)
FOR K = 1 TO N
   ID = IDS<K>
   READ R FROM F, ID ELSE R = ""
   * gather each column's raw value and its value count
   CVAL = ""
   VCNT = ""
   FOR C = 1 TO CN
      BEGIN CASE
      CASE ANOS<C> = 0
         V = ID
      CASE ANOS<C> = -1
         V = IEVAL(R, ISPECS<C>)
      CASE 1
         V = R<ANOS<C>>
      END CASE
      CVAL<C> = V
      VCNT<C> = DCOUNT(V, @VM)
   NEXT C
   * a column in an association takes its value count from the
   * controlling member (the one with the lowest attribute number), so
   * dependents align to it.  NV is the tallest column: one sub-row per
   * value, single-valued columns shown once and blank thereafter.
   NV = 1
   FOR C = 1 TO CN
      EC = VCNT<C>
      IF ASSOCS<C> # "" THEN
         CTL = C
         FOR C2 = 1 TO CN
            IF ASSOCS<C2> = ASSOCS<C> AND ANOS<C2> < ANOS<CTL> THEN
               CTL = C2
            END
         NEXT C2
         EC = VCNT<CTL>
      END
      ECNT<C> = EC
      IF EC > NV THEN
         NV = EC
      END
   NEXT C
   FOR SR = 1 TO NV
      IF SR = 1 THEN
         ROW = FMT(ID, "L#12")
      END ELSE
         ROW = FMT("", "L#12")
      END
      FOR C = 1 TO CN
         IF SR <= ECNT<C> THEN
            V = FIELD(CVAL<C>, @VM, SR)
         END ELSE
            V = ""
         END
         IF V # "" AND CONVS<C> # "" THEN
            V = OCONV(V, CONVS<C>)
         END
         ROW = ROW:" ":FMT(V, MASKS<C>)
      NEXT C
      PRINT ROW
   NEXT SR
NEXT K
PRINT N:" record(s) listed"
STOP

