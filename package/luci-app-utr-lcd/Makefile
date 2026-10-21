include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-utr-lcd
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

PKG_MAINTAINER:=Roman Belyakovsky <roman.beliakovskii@smartly.io>
PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=LICENSE

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-utr-lcd
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI support for the UniFi Travel Router LCD (utr-lcd)
  DEPENDS:=+luci-base +utr-lcd
  PKGARCH:=all
endef

define Package/luci-app-utr-lcd/description
  Settings and status page (Services > LCD Display) and screens editor with live preview
  (Services > LCD Screens) for the utr-lcd LCD status display.
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/luci-app-utr-lcd/install
	$(INSTALL_DIR) $(1)/usr/libexec/utr-lcd $(1)/usr/share/luci/menu.d $(1)/usr/share/rpcd/acl.d \
		$(1)/www/luci-static/resources/view/utr-lcd
	$(INSTALL_BIN) ./root/usr/libexec/utr-lcd/luci-status $(1)/usr/libexec/utr-lcd/luci-status
	$(INSTALL_BIN) ./root/usr/libexec/utr-lcd/luci-preview $(1)/usr/libexec/utr-lcd/luci-preview
	$(INSTALL_DATA) ./root/usr/share/luci/menu.d/luci-app-utr-lcd.json $(1)/usr/share/luci/menu.d/
	$(INSTALL_DATA) ./root/usr/share/rpcd/acl.d/luci-app-utr-lcd.json $(1)/usr/share/rpcd/acl.d/
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/utr-lcd/settings.js \
		$(1)/www/luci-static/resources/view/utr-lcd/settings.js
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/utr-lcd/screens.js \
		$(1)/www/luci-static/resources/view/utr-lcd/screens.js
endef

define Package/luci-app-utr-lcd/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || { rm -f /tmp/luci-indexcache* ; /etc/init.d/rpcd reload 2>/dev/null; }
exit 0
endef

$(eval $(call BuildPackage,luci-app-utr-lcd))
