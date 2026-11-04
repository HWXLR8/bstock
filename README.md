# bstock

Portfolio quotes from Yahoo Finance. Build and run:

    make
    ./bstock

Reads `$BSTOCK_HOME/bstock.json` (default `./bstock.json`).

## Config

    {
      "tickers": [
        { "symbol": "VT", "shares": 200 },
        { "symbol": "GOOG", "shares": 100 },
        { "symbol": "NFLX", "shares": 160 }
      ]
    }
