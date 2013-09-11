<xsl:stylesheet version = '1.0'
     xmlns:xsl='http://www.w3.org/1999/XSL/Transform'>
  <xsl:output indent="yes"
              doctype-system="http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd"
              doctype-public="-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"/>
  <xsl:template match="/node">
    <busconfig>
      <xsl:apply-templates />
    </busconfig>
  </xsl:template>

  <xsl:template match="//interface[@name]">
    <xsl:variable name="if"><xsl:value-of select="@name"/></xsl:variable>
    <policy user="root">
      <allow>
        <xsl:attribute name="own">
          <xsl:value-of select="$if"/>
        </xsl:attribute>
      </allow>
      <allow>
        <xsl:attribute name="send_interface">
          <xsl:value-of select="$if"/>
        </xsl:attribute>
      </allow>
    </policy>

    <policy at_console="true">
      <allow>
        <xsl:attribute name="send_interface">
          <xsl:value-of select="$if"/>
        </xsl:attribute>
      </allow>
      <allow send_interface="org.freedesktop.DBus.Introspectable">
        <xsl:attribute name="send_destination">
          <xsl:value-of select="$if"/>
        </xsl:attribute>
      </allow>
      <allow send_interface="org.freedesktop.DBus.Properties">
        <xsl:attribute name="send_destination">
          <xsl:value-of select="$if"/>
        </xsl:attribute>
      </allow>
    </policy>

    <policy context="default">
      <deny>
        <xsl:attribute name="send_interface">
          <xsl:value-of select="$if"/>
        </xsl:attribute>
      </deny>
    </policy>
  </xsl:template>

  <!-- this strips out all the non-tag text so that
       we don't emit lots of unwanted inter-tag whitespace  -->
  <xsl:template match="text()"/>

</xsl:stylesheet> 
