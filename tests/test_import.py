def test_import_extension_before_package():
    import posixipc._posixipc as ext

    assert isinstance(ext.__build_info__, dict)
    assert "have_robust_mutex" in ext.__build_info__


def test_import_package():
    import posixipc

    assert posixipc.features is not None
    assert posixipc.__build_info__ is posixipc._posixipc.__build_info__
