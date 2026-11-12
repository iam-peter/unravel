A small Python package with known flaws, for trying unravel on Python:
`shop.catalog` and `shop.orders` import each other, `slugify` is written
twice, `legacy_total` is used nowhere, and `Order.summary` is too complex.
