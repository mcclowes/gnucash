# Bank feeds and HMRC Making Tax Digital

Two connections out of GnuCash, built the same way: GnuCash talks to the
service directly, with credentials the user owns, and nothing sits in
between.

- **Bank feeds** (*Actions → Bank Feeds…*) pull transactions from UK banks
  through Open Banking, via [Plaid](https://plaid.com/docs/api/).
- **VAT returns** (*Business → VAT Returns (HMRC MTD)…*) work out the nine
  boxes from the book and file them with HMRC's
  [VAT (MTD) API](https://developer.service.hmrc.gov.uk/api-documentation/docs/api/service/vat-api/1.0).

Both are built when `WITH_ONLINE_SERVICES` is on (the default) and libcurl
is available. The embedded sign-in window needs WebKit2GTK, which GnuCash
already uses for reports.

## Layout

| File | What it does |
|---|---|
| `gnc-json.*` | A small JSON model that keeps numbers as their literal text. Money never passes through a `double`. |
| `gnc-online-http.*` | The `HttpTransport` seam (libcurl in production, scripted in tests). |
| `gnc-online-secrets.*` | Credentials: libsecret when a secret service answers, otherwise a `0600` file in the user data directory. Never the book. |
| `gnc-online-book.*` | Per-book state (mappings, cursors, receipts) as JSON under the book's `online/` KVP slot. |
| `gnc-hmrc.*` | OAuth, obligations, returns, liabilities, payments, sandbox helpers, and the fraud prevention headers. |
| `gnc-vat-return.*` | From splits to boxes; the filing record; the settlement journal. |
| `gnc-plaid.*` | Hosted Link, token exchange, accounts, `/transactions/sync`. |
| `gnc-bank-feeds.*` | Connections, account mapping, and turning feed rows into transactions for the import matcher. |
| `dialog-*.cpp`, `gnc-online-ui.*`, `gnc-plugin-online.*` | The windows and menu entries. |
| `test/` | Offline tests: protocol shapes against a scripted transport, box arithmetic against a real in-memory book. |

The engine-facing code (`gnc-online-core`) has no GTK dependency; the
windows live in `gnc-online`.

## VAT: how the boxes are worked out

Accounts are assigned to boxes on the *VAT Boxes* page; sub-accounts
follow their parent. For a period, on the accruals basis:

- **Box 1, Box 2** count credits as positive. **Box 4** counts debits.
- One account may hold both sides (Box 1 or 2, plus Box 4), for books
  with a single "VAT" account. Each split then goes by what its
  transaction is doing: a customer credit note (income debited) reduces
  Box 1, a supplier credit note (expense credited) reduces Box 4, and
  otherwise credits go to Box 1 or 2 and debits to Box 4. A Northern
  Ireland reverse charge in one account therefore fills Box 2 and Box 4.
- **Box 6, Box 8** count credits; **Box 7, Box 9** count debits. Box 6
  includes Box 8 and Box 7 includes Box 9, as HMRC requires. They are
  truncated to whole pounds.
- **Box 3** = Box 1 + Box 2. **Box 5** = |Box 3 − Box 4|.
- Transactions that settle with HMRC are left out: anything touching the
  settlement account, the journal GnuCash posts when filing, and anything
  that moves money only between *output* VAT accounts and bank, cash or
  card accounts. Input VAT paid straight from the bank (import VAT on a
  C79, say) still counts. Excluded transactions are listed in the window, never
  dropped silently.
- **Flat Rate Scheme**: Box 6 becomes gross turnover and Box 1 is that
  times the flat rate, to the penny.

Every figure opens to show the transactions behind it. Nothing is sent
until the user ticks HMRC's declaration and presses *File*. The return
is worked out again at that moment; if the book has changed since it
was shown, nothing is sent and the new figures are put up for review.

If HMRC accepted a filing but the reply never arrived, the retry comes
back as a duplicate. GnuCash then fetches HMRC's copy of the return,
records it under *Filed*, and says whether it matches the book. It does
not post a settlement journal in that case. The receipt
(form bundle number, charge reference, `Receipt-ID`, correlation id,
timestamp and signature) is stored in the book under *Filed*.

Cash accounting is not supported: the period is taken by posted date.

## Setting up HMRC

1. Register at the [HMRC Developer Hub](https://developer.service.hmrc.gov.uk/),
   create an application and subscribe it to **VAT (MTD)**.
2. Add the redirect address shown on the *Connection* page
   (`http://localhost:8765/gnucash/hmrc-callback` by default). Nothing
   listens there: the sign-in window watches for the redirect and takes
   the code from it.
3. Paste the client id and secret, enter the VAT number, and *Sign in with
   HMRC*.
4. In the sandbox, *Create test business* makes a test user and VAT
   number, and *Check fraud prevention headers* asks HMRC to validate
   what GnuCash sends.

Production credentials are granted by HMRC after its own review of the
application. A sandbox application cannot file real returns.

### Fraud prevention headers

GnuCash connects as `DESKTOP_APP_DIRECT` and sends the device id (a random
UUID kept in the user data directory), OS user, time zone, local IPs with
timestamp, MAC addresses, screens, window size, OS and device details, and
the vendor product name and version. Headers it cannot honestly fill are
omitted rather than sent blank. `Gov-Client-Multi-Factor` and
`Gov-Vendor-License-IDs` are always omitted, because GnuCash has
neither. HMRC's validator may flag the missing licence header, and
device manufacturer and model are only read on Linux. Run *Check fraud
prevention headers* in the sandbox and settle any gaps with HMRC before
asking for production credentials.

## Setting up bank feeds

1. Sign up at the [Plaid dashboard](https://dashboard.plaid.com/), and in
   *Bank Feeds → ☰ → Plaid Keys…* paste the client id and the secret for
   the environment. The sandbox is free and has test banks; production
   access for UK banks needs Plaid's approval.
2. *Connect a Bank…* opens Plaid Hosted Link in a private, cookie-less
   browser window. Bank credentials go to the bank, never to GnuCash.
3. Choose the GnuCash account each bank account feeds, or switch its
   import off. GnuCash guesses by name and the last four digits.
4. *Sync* downloads new transactions and opens the standard import
   matcher. Duplicates are recognised by the bank's transaction id
   (stored as the split's online id), and the matcher's learned account
   guesses apply as usual.

The sync cursor only moves once the import is accepted, so cancelling the
review loses nothing. Pending card transactions are skipped by default;
they arrive when they clear. *Download Full History Again…* resets the
cursor to recover anything skipped in an earlier review; transactions
already in the book are recognised and left alone.

With pending transactions switched on, a pending row that later posts
keeps its place: the imported split takes the posted transaction's id
instead of being imported twice. Changes the bank makes to rows already
imported, and rows it withdraws, are listed after each sync for you to
check; GnuCash does not edit imported transactions itself.

Each connection card compares the bank's balance with the book's, which
is the quickest check that nothing has been missed.

## Testing

```
ninja test-online-json test-online-hmrc test-online-plaid \
      test-online-vat-return test-online-bank-feeds
ctest -R online
```

The tests make no network calls. The HMRC and Plaid clients run against a
scripted transport, which checks request URLs, headers and bodies and
replays documented response shapes, including errors, pagination and
Plaid's mutation-during-pagination restart. The VAT tests build a small
sole-trader book and check every box, the single-account split, the Flat
Rate Scheme, repayments, exclusions and the settlement journal.
